// File $Id$
// Author: John Wu <John.Wu@ACM.org>
//         Lawrence Berkeley National Laboratory
// Copyright 2000-2008 the Regents of the University of California
//
//  The implementation of class query.  It performs most of the query
//  processing functions, calls the table object form the actual estimation
//  work.
//
#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)	// some identifier longer than 256 characters
#endif
#include "query.h"	// class query (prototypes for all functions here)
#include "predicate.h"	// definitions use when parsing queries
#include "bundle.h"	// class bundle
#include "ibin.h"	// ibis::bin
#include "iroster.h"	// ibis::roster, used by ibis::table::vault
#include "irelic.h"	// ibis::join::estimate
#include "bitvector64.h"

#include <stdio.h>	// remove()
#include <stdarg.h>	// vsprintf
#include <ctype.h>	// isspace, tolower

#include <set>		// std::set
#include <algorithm>	// std::sort, std::swap
#include <sstream>	// std::ostringstream

// delimiters that can be used to separate names in a name list
const char* ibis::util::delimiters = ";, \b\f\r\t\n'\"";

namespace ibis {
#if defined(TEST_SCAN_OPTIONS)
    extern int _scan_option;
#endif
}

void ibis::nameList::select(const char* str) {
    if (str == 0) return;
    if (*str == static_cast<char>(0)) return;

    // first put the incoming string into a list of strings
    std::set<std::string> strlist;
    const char* s = str;
    const char* t = 0;
    do {
	s += strspn(s, ibis::util::delimiters); // remove leading space
	if (*s) {
	    t = strpbrk(s, ibis::util::delimiters);
	    if (t) { // found a delimitor
		std::string tmp;
		while (s < t) {
		    tmp += tolower(*s);
		    ++ s;
		}
		strlist.insert(tmp);
	    }
	    else { // no more delimitor
		std::string tmp;
		while (*s) {
		    tmp += tolower(*s);
		    ++ s;
		}
		strlist.insert(tmp);
	    }
	}
    } while (s != 0 && *s != 0);

    if (! strlist.empty()) {
	clear(); // clear existing content
	uint32_t tot = strlist.size();
	std::set<std::string>::const_iterator it;
	for (it = strlist.begin(); it != strlist.end(); ++ it)
	    tot += it->size();

	buff = new char[tot];
	cstr = new char[tot];

	it = strlist.begin();
	strcpy(buff, it->c_str());
	strcpy(cstr, it->c_str());
	cvec.push_back(buff);
	char* s1 = buff + it->size();
	char* t1 = cstr + it->size();
	for (++ it; it != strlist.end(); ++ it) {
	    ++ s1;
	    *t1 = ',';
	    ++ t1;
	    strcpy(s1, it->c_str());
	    strcpy(t1, it->c_str());
	    cvec.push_back(s1);
	    s1 += it->size();
	    t1 += it->size();
	}
    }
} // ibis::nameList::select

// the list of names are sorted
size_t ibis::nameList::find(const char* key) const {
    const size_t sz = cvec.size();
    if (sz < 8) { // linear search
	for (uint32_t i = 0; i < sz; ++ i) {
	    int tmp = stricmp(cvec[i], key);
	    if (tmp == 0) {
		return i;
	    }
	    else if (tmp > 0) {
		return sz;
	    }
	}
    }
    else { // binary search
	size_t i = 0;
	size_t j = sz;
	size_t k = (i + j) / 2;
	while (i < k) {
	    int tmp = stricmp(cvec[k], key);
	    if (tmp == 0) { // found a match
		return k;
	    }
	    else if (tmp < 0) {
		i = k + 1;
		k = (k + 1 + j) / 2;
	    }
	    else {
		j = k;
		k = (i + k) / 2;
	    }
	}
	if (i < j) {
	    if (0 == stricmp(cvec[i], key))
		return i;
	}
    }
    return sz;
} // ibis::nameList::find

// this function produces consistent result only for operators AND and OR
// the root cause of the problem is that is can not handle NOT operator
// correctly
double ibis::query::weight::operator()(const ibis::qExpr* ex) const {
    double res = dataset->nRows();
    switch (ex->getType()) {
    case ibis::qExpr::RANGE: {
	const ibis::qContinuousRange* tmp =
	    reinterpret_cast<const ibis::qContinuousRange*>(ex);
	if (tmp != 0)
	    res = dataset->estimateCost(*tmp);
	break;}
    case ibis::qExpr::DRANGE: {
	const ibis::qDiscreteRange* tmp =
	    reinterpret_cast<const ibis::qDiscreteRange*>(ex);
	if (tmp != 0)
	    res = dataset->estimateCost(*tmp);
	break;}
    case ibis::qExpr::STRING: {
	const ibis::qString* tmp =
	    reinterpret_cast<const ibis::qString*>(ex);
	if (tmp != 0)
	    res = dataset->estimateCost(*tmp);
	break;}
    default: { // most terms are evaluated through left and right children
	if (ex->getLeft()) {
	    res = operator()(ex->getLeft());
	    if (ex->getRight())
		res += operator()(ex->getRight());
	}
	else if (ex->getRight()) {
	    res = operator()(ex->getRight());
	}
	break;}
    } // switch
    return res;
} // ibis::query::weight::operator

///////////////////////////////////////////////////////////
// public functions of ibis::query
////////////////////////////////////////////////////////////

/// Integer error code:
///  0: successful completion of the requested operation.
/// -1: nil pointer to table or empty table.
/// -2: invalid string for select clause.
/// -3: select clause contains invalid column name.
/// -4: invalid string for where clause.
/// -5: where clause can not be parsed correctly.
/// -6: where clause contains invalid column names or unsupported functions.
/// -7: empty rid list for set rid operation.
/// -8: neither rids nor range conditions are set.
/// -9: encountered some exceptional conditions during query evaluations.
/// -10: no private directory to store bundles.
/// -11: Query not fully evaluated.
int ibis::query::setTable(const part* tbl) {
    if (tbl == 0) return -1;
    if (tbl == table0) return 0;
    if (tbl->nRows() == 0 || tbl->nColumns() == 0) return -1;

    writeLock control(this, "setTable");
    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	dstime = 0;
	if (hits == sup) {
	    delete hits;
	    hits = 0;
	    sup = 0;
	}
	else {
	    delete hits;
	    delete sup;
	    hits = 0;
	    sup = 0;
	}
	removeFiles();
    }

    table0 = tbl;
    std::vector<size_t> badnames;
    for (size_t i = 0; i < comps.size(); ++i) {
	if (0 == table0->getColumn(comps[i])) {
	    badnames.push_back(i);
	    logWarning("setTable", "table %s does not contain a "
		       "column named %s", table0->name(), comps[i]);
	}
    }
    if (!badnames.empty()) {
	logWarning("setTable", "The select clause \"%s\" contains %lu "
		   "invalid column name(s).  It will be removed",
		   *comps, static_cast<long unsigned>(badnames.size()));
	comps.remove(badnames);
    }

    if (expr == 0) {
	state = UNINITIALIZED;
    }
    else {
	int ierr = verifyPredicate(expr);
	if (ierr != 0) {
	    logWarning("setTable", "The WHERE clause \"%s\" contains "
		       "%d incorrect name(s). It will be removed",
		       (condition ? condition : "<long expression>"), ierr);
	    if (comps.size())
		state = SET_COMPONENTS;
	    else
		state = UNINITIALIZED;
	    delete [] condition;
	    condition = 0;
	    delete expr;
	    expr = 0;
	}
    }

    if (comps.size() != 0) {
	if (rids_in || expr) {
	    state = SPECIFIED;
	    writeQuery();
	}
	else {
	    state = SET_COMPONENTS;
	}
    }
    if (ibis::gVerbose > 0) {
	logMessage("setTable", "new dataset name %s",
		   table0->name());
    }
    return 0;
} // ibis::query::setTable

int ibis::query::setSelectClause(const char* str) {
    writeLock control(this, "setSelectClause");
    std::string oldComps = *comps;
    if (comps.size()) {
	if (str == 0 || *str == 0 ||
	    stricmp(str, oldComps.c_str()) != 0) { // different values
	    if (ibis::gVerbose > 2)
		logMessage("setSelectClause", "replace previous comps"
			   " \"%s\" with \"%s\".", oldComps.c_str(),
			   (str?str:""));
	}
	else {
	    return 0; // no change needed
	}
    }
    else if (str == 0 || *str == 0) {
	return 0; // no change needed
    }

    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	dstime = 0;
	if (hits == sup) {
	    delete hits;
	    hits = 0;
	    sup = 0;
	}
	else {
	    delete hits;
	    delete sup;
	    hits = 0;
	    sup = 0;
	}
	removeFiles();
    }

    comps.select(str);
    std::vector<size_t> badnames;
    for (size_t i = 0; table0 != 0 && i < comps.size(); ++i) {
	if (0 == table0->getColumn(comps[i])) {
	    badnames.push_back(i);
	    logWarning("setSelectClause", "table %s does not contain a "
		       "column named %s", table0->name(), comps[i]);
	}
    }
    if (! badnames.empty())
	comps.remove(badnames);
    if (comps.empty() && ! oldComps.empty()) {
	if (ibis::gVerbose > 0)
	    logMessage("setSelectClause", "The new Select clause \"%s\" "
		       "is empty after removing %lu unknown column name%s.  "
		       "Restore the old Select clause \"%s\"", str,
		       static_cast<long unsigned>(badnames.size()),
		       (badnames.size() > 1 ? "s" : ""), oldComps.c_str());
	comps.select(oldComps.c_str());
    }

    if (expr == 0) {
	state = UNINITIALIZED;
    }
    else {
	int ierr = verifyPredicate(expr);
	if (ierr != 0) {
	    logWarning("setSelectClause", "The WHERE clause \"%s\" "
		       "contains %d incorrect name(s). It will not be used",
		       (condition ? condition : "<long expression>"), ierr);
	    if (comps.size())
		state = SET_COMPONENTS;
	    else
		state = UNINITIALIZED;
	    delete [] condition;
	    condition = 0;
	    delete expr;
	    expr = 0;
	}
    }
    if (comps.size() != 0) {
	if (rids_in || condition) {
	    state = SPECIFIED;
	    writeQuery();
	}
	else {
	    state = SET_COMPONENTS;
	}
	if (ibis::gVerbose > 1) {
	    logMessage("setSelectClause", "SELECT %s", *comps);
	}
	return 0;
    }
    else {
	return -3;
    }
} // ibis::query::setSelectClause

/// The where clause is a string representing a list of range conditions.
/// A where clause is mandatory if a query is to be estimated or evaluated.
/// This function may be called multiple times and each invocation will
/// overwrite the previous where clause.
int ibis::query::setWhereClause(const char* str) {
    if (str == 0 || *str == static_cast<char>(0)) return -4;

    writeLock lck(this, "setWhereClause");
    char *oldCond = condition;
    qExpr *oldExpr = expr;
    if (condition) {
	if (stricmp(str, condition)) { // different values
	    if (ibis::gVerbose > 2)
		logMessage("setWhereClause", "replace previous condition "
			   "\"%s\" with \"%s\".", condition, str);
	}
	else {
	    return 0; // no change needed
	}
    }

    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	dstime = 0;
	if (hits == sup) {
	    delete hits;
	    hits = 0;
	    sup = 0;
	}
	else {
	    delete hits;
	    delete sup;
	    hits = 0;
	    sup = 0;
	}
	removeFiles();
    }

    expr = ibis::parseQuery(str);
    if (expr == 0) {
	logWarning("setWhereClause", "failed to parse the WHERE clause "
		   "\"%s\"", str);
	if (oldCond && oldExpr) {
	    condition = oldCond;
	    expr = oldExpr;
	}
	else if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	return -5;
    }
    else {
	condition = ibis::util::strnewdup(str);
	addJoinConstraints(expr); // add constraints derived from joins
	//ibis::qExpr::simplify(expr);
    }

    int ierr = verifyPredicate(expr);
    if (ierr != 0) {
	logWarning("setWhereClause", "The WHERE clause \"%s\" contains %d "
		   "incorrect name(s).  It will not be used.", str, ierr);
	if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	delete [] condition;
	condition = 0;
	delete expr;
	expr = 0;
	if (oldCond && oldExpr) {
	    condition = oldCond;
	    expr = oldExpr;
	}
	else if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	return -6;
    }

    // finally free the old where clause
    delete [] oldCond;
    delete oldExpr;

    if (comps.size()) {
	state = SPECIFIED;
	writeQuery();
    }
    else {
	state = SET_PREDICATE;
    }
    if (ibis::gVerbose > 0) {
	logMessage("setWhereClause", "WHERE \"%s\"", str);
	LOGGER(ibis::gVerbose >= 4) << "  Translated the WHERE clause into: " << *expr;
    }
    return 0;
} // ibis::query::setWhereClause

/// This function accepts a set of range conditions expressed by the three
/// vectors.  The arrays are expected to be of the same size, and each
/// triplet <names[i], lbounds[i], rbounds[i]> are interpreted as
/// @code
/// names[i] between lbounds[i] and rbounds[i]
/// @endcode
/// The range conditions are joined together with the AND operator.
/// If vectors lbounds and rbounds are not the same size, then the missing
/// one is consider to represent an open boundary.  For example, if
/// lbounds[4] exists but not rbounds[4], they the range condition is
/// interpreted as
/// @code
/// lbounds[4] <= names[4]
/// @endcode
int ibis::query::setWhereClause(const std::vector<const char*>& names,
				const std::vector<double>& lbounds,
				const std::vector<double>& rbounds) {
    size_t nts = names.size();
    if (rbounds.size() <= lbounds.size()) {
	if (nts > lbounds.size())
	    nts = lbounds.size();
    }
    else if (nts > rbounds.size()) {
	nts = rbounds.size();
    }
    if (nts == 0) return -4;

    writeLock lck(this, "setWhereClause");
    char *oldCond = condition;
    qExpr *oldExpr = expr;

    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	dstime = 0;
	if (hits == sup) {
	    delete hits;
	    hits = 0;
	    sup = 0;
	}
	else {
	    delete hits;
	    delete sup;
	    hits = 0;
	    sup = 0;
	}
	removeFiles();
    }

    if (lbounds.size() > 0) {
	if (rbounds.size() > 0) {
	    double lb = (lbounds[0] <= rbounds[0] ?
			 lbounds[0] : rbounds[0]);
	    double rb = (lbounds[0] <= rbounds[0] ?
			 rbounds[0] : lbounds[0]);
	    expr = new ibis::qContinuousRange(lb, ibis::qExpr::OP_LE, names[0],
					      ibis::qExpr::OP_LE, rb);
	}
	else {
	    expr = new ibis::qContinuousRange(names[0], ibis::qExpr::OP_GE,
					      lbounds[0]);
	}
    }
    else {
	expr = new ibis::qContinuousRange(names[0], ibis::qExpr::OP_LE,
					  rbounds[0]);
    }
    for (size_t i = 1; i < nts; ++ i) {
	ibis::qExpr *tmp = new ibis::qExpr(ibis::qExpr::LOGICAL_AND);
	tmp->setLeft(expr);
	expr = tmp;
	if (lbounds.size() > i) {
	    if (rbounds.size() > i) {
		double lb = (lbounds[i] <= rbounds[i] ?
			     lbounds[i] : rbounds[i]);
		double rb = (lbounds[i] <= rbounds[i] ?
			     rbounds[i] : lbounds[i]);
		tmp = new ibis::qContinuousRange(lb, ibis::qExpr::OP_LE,
						 names[i],
						 ibis::qExpr::OP_LE, rb);
	    }
	    else {
		tmp = new ibis::qContinuousRange(names[i], ibis::qExpr::OP_GE,
						 lbounds[i]);
	    }
	}
	else {
	    tmp = new ibis::qContinuousRange(names[i], ibis::qExpr::OP_LE,
					     rbounds[i]);
	}
	expr->setRight(tmp);
    }
#if defined(_DEBUG) || defined(DEBUG)
    if (expr != 0) {
	std::ostringstream ostr;
	ostr << *expr;
	condition = ibis::util::strnewdup(ostr.str().c_str());
    }
#else
    condition = 0;
#endif
    int ierr = verifyPredicate(expr);
    if (ierr != 0) {
	logWarning("setWhereClause", "The WHERE clause specified in three "
		   "arrays contain %d incorrect name(s). Revert to old values.",
		   ierr);
	if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	delete [] condition;
	condition = 0;
	delete expr;
	expr = 0;
	if (oldCond && oldExpr) {
	    condition = oldCond;
	    expr = oldExpr;
	}
	else if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	return -6;
    }

    // finally free the old where clause
    delete [] oldCond;
    delete oldExpr;

    if (comps.size()) {
	state = SPECIFIED;
	writeQuery();
    }
    else {
	state = SET_PREDICATE;
    }
    LOGGER(ibis::gVerbose >= 2) << "query[" << myID
	      << "]::setWhereClause converted three arrays to \""
	      << *expr << "\"";
    return 0;
} // ibis::query::setWhereClause

/// This function accepts a user constructed query expression object.  It
/// can be used to bypass call to ibis::parseQuery.
int ibis::query::setWhereClause(const ibis::qExpr* qx) {
    if (qx == 0) return -4;

    writeLock lck(this, "setWhereClause");
    char *oldCond = condition;
    qExpr *oldExpr = expr;

    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	dstime = 0;
	if (hits == sup) {
	    delete hits;
	    hits = 0;
	    sup = 0;
	}
	else {
	    delete hits;
	    delete sup;
	    hits = 0;
	    sup = 0;
	}
	removeFiles();
    }

    expr = qx->dup();
    if (expr == 0) {
	logWarning("setWhereClause",
		   "failed to duplicate the incoming ibis::qExpr object");
	if (oldCond && oldExpr) {
	    condition = oldCond;
	    expr = oldExpr;
	}
	else if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	return -5;
    }
    else {
#if defined(DEBUG) || defined(_DEBUG)
	std::ostringstream ostr;
	ostr << *expr;
	condition = ibis::util::strnewdup(ostr.str().c_str());
#else
	condition = 0;
#endif
	addJoinConstraints(expr); // add constraints derived from joins
	ibis::qExpr::simplify(expr);
    }

    int ierr = verifyPredicate(expr);
    if (ierr != 0) {
	logWarning("setWhereClause", "The WHERE clause expressed in the "
		   "qExpr object contains %d incorrect name(s).  Revert to "
		   "old value", ierr);
	if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	delete [] condition;
	condition = 0;
	delete expr;
	expr = 0;
	if (oldCond && oldExpr) {
	    condition = oldCond;
	    expr = oldExpr;
	}
	else if (comps.size())
	    state = SET_COMPONENTS;
	else
	    state = UNINITIALIZED;
	return -6;
    }

    // finally free the old where clause
    delete [] oldCond;
    delete oldExpr;

    if (comps.size()) {
	state = SPECIFIED;
	writeQuery();
    }
    else {
	state = SET_PREDICATE;
    }
    LOGGER(ibis::gVerbose >= 2) << "query[" << myID
	      << "]::setWhereClause accepted new query conditions \""
	      << *expr << "\"";
    return 0;
} // ibis::query::setWhereClause

// incoming RIDs are copied
int ibis::query::setRIDs(const ibis::RIDSet& rids) {
    if (rids.empty()) return -7;

    writeLock lck(this, "setRIDs");
    if (rids_in != 0) delete rids_in;
    rids_in = new RIDSet();
    rids_in->deepCopy(rids);
    std::sort(rids_in->begin(), rids_in->end());
    //    ibis::util::sortRIDs(*rids_in);

    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	dstime = 0;
	if (hits == sup) {
	    delete hits;
	    hits = 0;
	    sup = 0;
	}
	else {
	    delete hits;
	    delete sup;
	    hits = 0;
	    sup = 0;
	}
	removeFiles();
    }

    if (comps.size()) {
	writeQuery();
	state = SPECIFIED;
    }
    else {
	state = SET_RIDS;
    }
    if (ibis::gVerbose > 0)
	logMessage("setRIDs", "selected %lu RID(s) for an RID query",
		   static_cast<long unsigned>(rids_in->size()));
    return 0;
} // ibis::query::setRIDs

// quick estimate returns two bitvectors pointed by hits and sup
// where hits should contain no more set bits than that of sup
int ibis::query::estimate() {
    if (table0 == 0 || table0->nRows() == 0 || table0->nColumns() == 0)
	return -1;
    if (rids_in == 0 && expr == 0) { // not ready for this yet
	if (ibis::gVerbose > 1)
	    logMessage("estimate", "must have either a valid query "
		       "condition (the WHERE clause) or a list of RIDs");
	return -8;
    }
    if (ibis::gVerbose > 3) {
	logMessage("estimate", "starting to estimate query");
    }

    double pcnt = ibis::fileManager::instance().pageCount();
    if (dstime != 0 && dstime != table0->timestamp()) {
	// clear the current results and prepare for re-evaluation
	dstime = 0;
	if (hits == sup) {
	    delete hits;
	    hits = 0;
	    sup = 0;
	}
	else {
	    delete hits;
	    delete sup;
	    hits = 0;
	    sup = 0;
	}
	removeFiles();
	state = SPECIFIED;
    }
    if (state < QUICK_ESTIMATE) {
	writeLock lck(this, "estimate");
	if (state < QUICK_ESTIMATE) {
	    ibis::horometer timer;
	    if (ibis::gVerbose > 0)
		timer.start();
	    try {
		if (dslock == 0) { // acquire read lock on the dataset
		    dslock = new ibis::part::readLock(table0, myID);
		    dstime = table0->timestamp();
		}

#ifndef DONOT_REORDER_EXPRESSION
		if (! expr->directEval())
		    reorderExpr();
#endif
		getBounds(); // actual function to perform the estimation
		state = QUICK_ESTIMATE;
	    }
	    catch (const ibis::bad_alloc& e) {
		if (dslock) {
		    delete dslock;
		    dslock = 0;
		}

		LOGGER(ibis::gVerbose >= 0)
		    << " Error *** ibis::query[" << myID << "]::estimate("
		    << (condition ? condition : expr ? "<long expression>":
			"<RID query>")
		    << ") failed due to a memory allocation problem -- "
		    << e.what();
		return -9;
	    }
	    catch (const std::exception& e) {
		if (dslock) {
		    delete dslock;
		    dslock = 0;
		}

		LOGGER(ibis::gVerbose >= 0)
		    << " Error *** ibis::query[" << myID << "]::estimate("
		    << (condition ? condition : expr ? "<long expressioin>" :
			"<RID query>")
		    << ") failed -- " << e.what();
		return -9;
	    }
	    catch (const char* s) {
		if (dslock) {
		    delete dslock;
		    dslock = 0;
		}

		LOGGER(ibis::gVerbose >= 0)
		    << " Error *** ibis::query[" << myID << "]::estimate("
		    << (condition ? condition : expr ? "<long expression>" :
			"<RID query>")
		    << ") failed -- " << s;
		return -9;
	    }
	    catch (...) {
		if (dslock) {
		    delete dslock;
		    dslock = 0;
		}

		LOGGER(ibis::gVerbose >= 0)
		    << " Error *** ibis::query[" << myID << "]::estimate("
		    << (condition ? condition : expr ? "<long expression>" :
			"<RID query>")
		    << ") failed due to a unexpected exception ";
		return -9;
	    }
	    if (ibis::gVerbose > 0) {
		timer.stop();
		logMessage("estimate", "time to compute the bounds: "
			   "%g sec(CPU) and %g sec(elapsed).", timer.CPUTime(),
			   timer.realTime());
	    }
	}
    }

    if (hits==0 && sup==0) {
	logWarning("estimate", "unable to generate estimated hits");
    }
    else if (ibis::gVerbose > 0) {
	if (expr) {
	    if (expr->hasJoin())
		/*logMessage("estimate", "")*/;
	    else if (hits && sup && hits != sup) {
		logMessage("estimate", "# of hits for query \"%s\" is in "
			   "[%lu, %lu]", (condition ? condition :
					  "<long expression>"),
			   static_cast<long unsigned>(hits->cnt()),
			   static_cast<long unsigned>(sup->cnt()));
	    }
	    else if (hits) {
		logMessage("estimate", "# of hits for query \"%s\" is %lu",
			   (condition ? condition : "<long expression>"),
			    static_cast<long unsigned>(hits->cnt()));
	    }
	    else {
		hits = new ibis::bitvector;
		hits->set(0, sup->size());
		logWarning("estimate", "the lower bound is expected to be "
			   "computed, but it is not!\n# of hits is likely "
			   "in the range of [0, %lu]",
			   static_cast<long unsigned>(sup->cnt()));
	    }
	}
	else {
	    logMessage("estimate", "# of hits for the OID query is %lu",
		       static_cast<long unsigned>(hits->cnt()));
	}
	if (ibis::gVerbose > 4) {
	    pcnt = ibis::fileManager::instance().pageCount() - pcnt;
	    if (pcnt > 0.0)
		logMessage("estimate", "read(unistd.h) accessed "
			   "%g pages during the execution of this function",
			   pcnt);
	}
	if ((expr == 0 || ! expr->hasJoin()) &&
	    (ibis::gVerbose >= 30 ||
	     (ibis::gVerbose > 8 &&
	      (1U<<ibis::gVerbose) >= (hits?hits->bytes():0)+
	      (sup?sup->bytes():0)))) {

	    if (hits == sup) {
		LOGGER(ibis::gVerbose >= 0) << "The hit vector" << *hits;
	    }
	    else {
		if (hits)
		    LOGGER(ibis::gVerbose >= 0) << "The sure hits" << *hits;
		if (sup)
		    LOGGER(ibis::gVerbose >= 0) << "The possible hit" << *sup;
	    }
	}
    }
    return 0;
} // ibis::query::estimate

long ibis::query::getMinNumHits() const {
    readLock lck(this, "getMinNumHits");
    long nHits = (hits != 0 ? static_cast<long>(hits->cnt()) : -1);
    if (ibis::gVerbose > 11)
	logMessage("getMinNumHits", "minHits = %d", nHits);

    return nHits;
}

long ibis::query::getMaxNumHits() const {
    readLock lck(this, "getMaxNumHits");
    long nHits = (sup != 0 ? static_cast<long>(sup->cnt()) :
		 (hits ? static_cast<long>(hits->cnt()) : -1));
    if (ibis::gVerbose > 11)
	logMessage("getMaxNumHits", "maxHits = %d", nHits);
    return nHits;
}

// full estimate will generate the exact hit list (qualified events)
// if evalSelect is true, it will compute the RIDs of the qualified rows and
// the values of the columns specified in the select clause, and will write
// the values to disk for later use.
int ibis::query::evaluate(const bool evalSelect) {
    if (table0 == 0 || table0->nRows() == 0 || table0->nColumns() == 0)
	return -1;
    if (rids_in == 0 && expr == 0) {
	if (ibis::gVerbose > 1)
	    logMessage("evaluate", "must have either a WHERE clause "
		       "or a RID list");
	return -8;
    }
    if (ibis::gVerbose > 3) {
	logMessage("evaluate", "starting to evaluate the query for "
		   "user \"%s\"", user);
    }

    int ierr;
    ibis::horometer timer;
    double pcnt = ibis::fileManager::instance().pageCount();
    writeLock lck(this, "evaluate");
    if ((state < FULL_EVALUATE) ||
	(dstime != 0 && dstime != table0->timestamp())) {
	if (dstime != 0 && dstime != table0->timestamp()) {
	    // clear the current results and prepare for re-evaluation
	    dstime = 0;
	    if (hits == sup) {
		delete hits;
		hits = 0;
		sup = 0;
	    }
	    else {
		delete hits;
		delete sup;
		hits = 0;
		sup = 0;
	    }
	    removeFiles();
	    state = SPECIFIED;
	}
	if (ibis::gVerbose > 0)
	    timer.start();
	try {
	    if (dslock == 0) { // acquire read lock on the table0
		dslock = new ibis::part::readLock(table0, myID);
		dstime = table0->timestamp();
	    }

	    ierr = computeHits(); // do actual computation here
	    if (ierr < 0) return ierr;
	    if (hits != 0 && hits->cnt() > 0 && expr != 0) {
		if (ibis::gVerbose > 3) {
		    const unsigned nb = hits->size();
		    const unsigned nc = hits->cnt();
		    const unsigned sz = hits->bytes();
		    double cf = ibis::bitvector::clusteringFactor(nb, nc, sz);
		    double rw = ibis::bitvector::randomSize(nb, nc);
		    double eb = static_cast<double>(countPages(4))
			* ibis::fileManager::pageSize();
		    logMessage("evaluate", "the hit vector contains %u bit%s "
			       "with %u bit%s set(=1) taking %u byte%s; the "
			       "estimated clustering factor is %g; had the "
			       "bits been randomly spread out, the expected "
			       "size would be %g bytes; estimated number of "
			       "bytes to be read in order to access 4-byte "
			       "values is %g", nb, (nb>1 ? "s" : ""),
			       nc, (nc>1 ? "s" : ""), sz, (sz>1 ? "s" : ""),
			       cf, rw, eb);
		}
		if (expr->hasJoin()) {// has join conditions
		    if (myDir == 0)
			setMyDir(table0->name());
		    processJoin();
		}
	    }
	}
	catch (const ibis::bad_alloc& e) {
	    if (dslock) {
		delete dslock;
		dslock = 0;
	    }
	    LOGGER(ibis::gVerbose >= 0)
		<< " Error *** ibis::query[" << myID << "]::evaluate("
		<< (condition ? condition : expr ? "<long expression>" :
		    "<RID query>") << ") failed "
		<< "due to a memory allocation problem, " << e.what();
	    return -9;
	}
	catch (const std::exception& e) {
	    if (dslock) {
		delete dslock;
		dslock = 0;
	    }

	    LOGGER(ibis::gVerbose >= 0)
		<< " Error *** ibis::query[" << myID << "]::evaluate("
		<< (condition ? condition : expr ? "<long expression>" :
		    "<RID query>") << ") failed "
		<< "due to " << e.what();
	    return -9;
	}
	catch (const char *e) {
	    if (dslock) {
		delete dslock;
		dslock = 0;
	    }

	    LOGGER(ibis::gVerbose >= 0)
		<< " Error *** ibis::query[" << myID << "]::evaluate("
		<< (condition ? condition : expr ? "<long expression>" :
		    "<RID query>") << ") failed "
		<< "due to " << e;
	    return -9;
	}
	catch (...) {
	    if (dslock) {
		delete dslock;
		dslock = 0;
	    }
	    LOGGER(ibis::gVerbose >= 0)
		<< " Error *** ibis::query[" << myID << "]::evaluate("
		<< (condition ? condition : expr ? "<long expression>" :
		    "<RID query>") << ") failed "
		<< "due to a unknown exception ";
	    return -9;
	}
	if (ibis::gVerbose > 0) {
	    const long unsigned nhits = hits->cnt();
	    timer.stop();
	    logMessage("evaluate", "time to compute the %lu "
		       "hit%s: %g sec(CPU) and %g sec(elapsed).",
		       nhits, (nhits > 1 ? "s" : ""),
		       timer.CPUTime(), timer.realTime());
	}
	state = FULL_EVALUATE;
	writeQuery(); // record the current status
    }

    if (myDir && hits->cnt() > 0 && evalSelect) {
	// generate the bundles
	writeHits();
	if (ibis::gVerbose > 1) timer.start();
	ibis::bundle* bdl = ibis::bundle::create(*this);
	if (bdl != 0) {
	    bdl->write(*this);
	    delete bdl;
	    if (ibis::gVerbose > 1) {
		timer.stop();
		logMessage("evaluate", "time to read qualified values "
			   "and write to disk (%s) is "
			   "%g sec(CPU) and %g sec(elapsed).",
			   myDir,
			   timer.CPUTime(),
			   timer.realTime());
	    }
	}

	state = FULL_EVALUATE;
	writeQuery(); // record the current status
	if (ibis::gVerbose > 0) {
	    timer.stop();
	    logMessage("evaluate", "time to compute the %lu "
		       "hits: %g sec(CPU) and %g sec(elapsed).",
		       static_cast<long unsigned>(hits->cnt()),
		       timer.CPUTime(), timer.realTime());
	}
	else
	    logWarning("evaluate", "unable to construct ibis::bundle");
    }

    if (dslock) { // make sure the read lock on the data table is released
	delete dslock;
	dslock = 0;
    }
    if (state != FULL_EVALUATE) {
	logWarning("evaluate", "unable to compute the hit vector");
	return -9;
    }
    else if (hits == 0) {
	if (ibis::gVerbose > 0)
	    logMessage("evaluate", "nHits = 0.");
    }
    else if (ibis::gVerbose > 0) {
	if (expr != 0) {
	    if (expr->hasJoin())
		/*logMessage("evaluate", "")*/;
	    else if (comps.size() > 0)
		logMessage("evaluate", "user %s SELECT %s FROM %s WHERE "
			   "%s ==> %lu hit%s.", user, *comps,
			   table0->name(), (condition ? condition :
					    "<long expression>"),
			   static_cast<long unsigned>(hits->cnt()),
			   (hits->cnt()>1?"s":""));
	    else
		logMessage("evaluate", "user %s FROM %s WHERE %s ==> "
			   "%lu hit%s.", user, table0->name(),
			   (condition ? condition : "<long expression>"),
			   static_cast<long unsigned>(hits->cnt()),
			   (hits->cnt()>1?"s":""));
	}
	else {
	    logMessage("evaluate", "user %s RID list of %lu elements ==> "
		       "%lu hit%s.", user,
		       static_cast<long unsigned>(rids_in->size()),
		       static_cast<long unsigned>(hits->cnt()),
		       (hits->cnt()>1?"s":""));
	}
	if (ibis::gVerbose > 3) {
	    pcnt = ibis::fileManager::instance().pageCount() - pcnt;
	    if (pcnt > 0.0)
		logMessage("evaluate", "read(unistd.h) accessed "
			   "%g pages during the execution of this function",
			   pcnt);
	}
 	if ((expr == 0 || !expr->hasJoin()) &&
	    (ibis::gVerbose >= 30 ||
	     (ibis::gVerbose > 8 &&
	      (1U<<ibis::gVerbose) >= hits->bytes()))) {
	    LOGGER(ibis::gVerbose >= 9) << "The hit vector" << *hits;
	}
    }
    return 0;
} // ibis::query::evaluate

long int ibis::query::getNumHits() const {
    readLock lock(this, "getNumHits");
    long int nHits = (hits != 0 && (sup == 0 || sup == hits) ?
		      static_cast<long int>(hits->cnt()) : -1);
    if (ibis::gVerbose > 11)
	logMessage("getNumHits", "nHits = %ld", nHits);
    return nHits;
} // ibis::query::getNumHits

// Caution: This function does not obtain a read lock on the query or the
// table.  Call it at your own risk.
long ibis::query::countHits() const {
    long int ierr = -1;
    if (hits != 0 && (sup == 0 || sup == hits))
	ierr = hits->cnt();
    else if (table0 != 0 && expr != 0 &&
	     (expr->getType() == ibis::qExpr::RANGE ||
	      expr->getType() == ibis::qExpr::DRANGE))
	ierr = table0->countHits(*static_cast<ibis::qRange*>(expr));
    return ierr;
} // ibis::query::countHits

int ibis::query::orderby(const char *names, int direction) const {
    if (myDir == 0)
	return -10;
    if (state != FULL_EVALUATE || state != BUNDLES_TRUNCATED
	|| state != HITS_TRUNCATED)
	return -11;
    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();
    ibis::bundle *bdl = ibis::bundle::create(*this);
    if (bdl != 0) {
	bdl->reorder(names, direction);
	bdl->write(*this);
	delete bdl;
    }
    else {
	logWarning("orderby", "failed to create bundles");
	return -12;
    }
    if (ibis::gVerbose > 2) {
	timer.stop();
	logMessage("orderby", "reordered according to %s using %g sec(CPU) "
		   "and %g sec(elapsed)", names, timer.CPUTime(),
		   timer.realTime());
    }
    return 0;
} // ibis::query::orderby

long int ibis::query::limit(const char *names, int direction, uint32_t keep,
			    bool updateHits) {
    long int ierr = 0;
    if (myDir == 0)
	return -10L;

    if (state == UNINITIALIZED || state == SET_COMPONENTS ||
	state == SET_RIDS || state == SET_PREDICATE) {
	ierr = -8;
	return ierr;
    }

    if (state == SPECIFIED || state == QUICK_ESTIMATE) {
	evaluate(true);
    }
    if (state != FULL_EVALUATE && state != BUNDLES_TRUNCATED &&
	state != HITS_TRUNCATED) {
	// failed to evaluate the query
	ierr = -9;
	return ierr;
    }

    ibis::horometer timer;
    if (ibis::gVerbose > 1)
	timer.start();
    ibis::bundle *bdl = ibis::bundle::create(*this);
    if (bdl != 0) {
	const uint32_t oldsize = bdl->size();
	ierr = bdl->truncate(names, direction, keep);
	if (ierr >= 0 && oldsize >= static_cast<long unsigned>(ierr)) {
	    if (updateHits) {
		ierr = table0->evaluateRIDSet(*(bdl->getRIDs()), *hits);
		state = HITS_TRUNCATED;
	    }
	    else {
		state = BUNDLES_TRUNCATED;
	    }
	    bdl->write(*this);
	}
	delete bdl;
	if (ibis::gVerbose > 1) {
	    timer.stop();
	    logMessage("limit", "reordered according to %s using %g sec(CPU) "
		       "and %g sec(elapsed), saved %ld bundles", names,
		       timer.CPUTime(), timer.realTime(), ierr);
	}
    }
    else  {
	logWarning("limit", "failed to create bundles");
	ierr = -12;
    }
    return ierr;
} // ibis::query::limit

// Compute the RIDs of the hits.  If the file rids exists, return the
// content of the file, else if the query was processed on the same data,
// call the dataset object to retrieve the RIDs, otherwise it will be NULL.
ibis::RIDSet* ibis::query::getRIDs() const {
    if (table0 == 0 || table0->hasRIDs() == false)
	return 0;
    if (state != FULL_EVALUATE) {
	logWarning("getRIDs", "call evaluate() first");
	return 0;
    }

    readLock lck(this, "getRIDs");
    ibis::RIDSet* rids = readRIDs();
    bool gotRIDs = true;
    if (rids == 0) {
	gotRIDs = false;
    }
    else if (rids->size() == hits->cnt()) {
	ibis::RIDSet* tmp = rids;
	rids = new ibis::RIDSet;
	rids->deepCopy(*tmp);
	delete tmp;
    }
    else {
	gotRIDs = false;
	delete rids;
	rids = 0;
    }

    if (gotRIDs == false) { // need to get RIDs from the table object
	ibis::part::readLock rock(table0, "getRIDs");
	if (hits && (dstime == table0->timestamp() || dstime == 0)) {
	    rids = table0->getRIDs(*hits);
	    writeRIDs(rids);
	    if (rids->size() != hits->cnt())
		logWarning("getRIDs", "retrieved %lu row IDs, but "
			   "expect %lu",
			   static_cast<long unsigned>(rids->size()),
			   static_cast<long unsigned>(hits->cnt()));
	    else if (ibis::gVerbose > 5)
		logMessage("getRIDs", "retrieved %lu row IDs "
			   "(hits->cnt() = %lu)",
			   static_cast<long unsigned>(rids->size()),
			   static_cast<long unsigned>(hits->cnt()));
	}
	else {
	    logWarning("getRIDs", "database has changed, "
		       "re-evaluate the query");
	}
    }

    if (ibis::gVerbose > 6 && rids != 0)
	logMessage("getRIDs", "numRIDs = %lu",
		   static_cast<long unsigned>(rids->size()));
    return rids;
} // ibis::query::getRIDs

// During the full estimate, query object is expected to write down the
// bundles and the RIDs of qualified events in each file bundle.  This
// function returns the RID set of the index'th (first one is zero'th) file
// bundle.
const ibis::RIDSet* ibis::query::getRIDsInBundle(const uint32_t bid) const {
    const ibis::RIDSet *rids = 0;
    if (comps.empty() || hits == 0 || hits->cnt() == 0)
	return rids;
    if (state != ibis::query::FULL_EVALUATE ||
	timestamp() != partition()->timestamp()) {
	logWarning("getRIDsInBundle", "query not fully evaluated or the "
		   "table has changed since last evaluation.  Need to "
		   "call evaluate again.");
	return rids;
    }

    bool noBundles = true;
    if (myDir != 0) {
	char* name = new char[strlen(myDir)+16];
	sprintf(name, "%s%cbundles", myDir, DIRSEP);
	noBundles = (ibis::util::getFileSize(name) == 0);
	delete [] name;
    }
    if (noBundles) { // attempt to create the bundles if no record of them
	const bool newlock = (dslock == 0);
	if (newlock) {
	    dslock = new ibis::part::readLock(partition(), id());
	}
	ibis::bundle* bdtmp = ibis::bundle::create(*this);
	if (newlock) {
	    delete dslock;
	    dslock = 0;
	}

	if (bdtmp != 0) {
	    if (ibis::gVerbose > 3)
		logMessage("getRIDsInBundle",
			   "successfully created file bundles");
	    rids = bdtmp->getRIDs(bid);
	    bdtmp->write(*this);
	    delete bdtmp;
	}
	else {
	    logWarning("getRIDsInBundle", "unable to genererate bundle");
	}
    }
    else {
	ibis::query::readLock lck2(this, "getRIDsInBundle");
	rids = ibis::bundle::readRIDs(myDir, bid);
    }
    if (ibis::gVerbose > 3) {
	if (rids != 0)
	    logMessage("getRIDsInBundle", "got %lu RID%s for file bundle %lu",
		       static_cast<long unsigned>(rids->size()),
		       (rids->size()>1?"s":""),
		       static_cast<long unsigned>(bid));
	else
	    logMessage("getRIDsInBundle", "got no RIDs for file bundle %lu",
		       static_cast<long unsigned>(bid));
    }
    return rids;
} // ibis::query::getRIDsInBundle

// Compute the row IDs of those marked 1 in the mask.
ibis::RIDSet* ibis::query::getRIDs(const ibis::bitvector& mask) const {
    ibis::RIDSet* ridset = 0;
    if (table0 == 0 || table0->hasRIDs() == false)
	return ridset;

    ibis::part::readLock tmp(table0, myID);
    ridset = table0->getRIDs(mask);
    if (ridset == 0 || ridset->size() != mask.cnt())
	logWarning("getRIDs", "got %lu row IDs from table %s, expected %lu",
		   static_cast<long unsigned>(ridset != 0 ? ridset->size() : 0),
		   table0->name(),
		   static_cast<long unsigned>(mask.cnt()));
    else if (ibis::gVerbose > 5)
	logMessage("getRIDs", "retrieved %lu row IDs from table %s",
		   static_cast<long unsigned>(ridset!=0 ? ridset->size() : 0),
		   table0->name());
    return ridset;
} // ibis::query::getRIDs

array_t<int32_t>* ibis::query::getQualifiedInts(const char* colname) {
    if (state != FULL_EVALUATE || dstime != table0->timestamp())
	evaluate();
    array_t<int32_t>* res = 0;
    if (dstime == table0->timestamp() && hits != 0) {
	readLock lck0(this, "getQualifiedInts");
	res = table0->selectInts(colname, *hits);
	if (ibis::gVerbose > 2)
	    logMessage("getQualifiedInts", "got %lu integer value(s)",
		       static_cast<long unsigned>(res != 0 ? res->size(): 0));
    }
    return res;
} // ibis::query::getQualifiedInts

array_t<uint32_t>* ibis::query::getQualifiedUInts(const char* colname) {
    if (state != FULL_EVALUATE || dstime != table0->timestamp())
	evaluate();
    array_t<uint32_t>* res = 0;
    if (dstime == table0->timestamp() && hits != 0) {
	readLock lck0(this, "getQualifiedUInts");
	res = table0->selectUInts(colname, *hits);
	if (ibis::gVerbose > 2)
	    logMessage("getQualifiedUInts", "got %lu integer value(s)",
		       static_cast<long unsigned>(res != 0 ? res->size() : 0));
    }
    return res;
} // ibis::query::getQualifiedUInts

array_t<float>* ibis::query::getQualifiedFloats(const char* colname) {
    if (state != FULL_EVALUATE || dstime != table0->timestamp())
	evaluate();
    array_t<float>* res = 0;
    if (dstime == table0->timestamp() && hits != 0) {
	const bool newlock = (dslock == 0);
	if (newlock) {
	    dslock = new ibis::part::readLock(table0, myID);
	}
	readLock lck(this, "getQualifiedFloats");
	res = table0->selectFloats(colname, *hits);

	if (newlock) {
	    delete dslock;
	    dslock = 0;
	}
	if (ibis::gVerbose > 2)
	    logMessage("getQualifiedFloats", "got %lu float value(s)",
		       static_cast<long unsigned>(res!=0 ? res->size() : 0));
    }
    return res;
} // ibis::query::getQualifiedFloats

array_t<double>* ibis::query::getQualifiedDoubles(const char* colname) {
    if (state != FULL_EVALUATE || dstime != table0->timestamp())
	evaluate();
    array_t<double>* res = 0;
    if (dstime == table0->timestamp() && hits != 0) {
	const bool newlock = (dslock == 0);
	if (newlock) {
	    dslock = new ibis::part::readLock(table0, myID);
	}
	readLock lck(this, "getQualifiedDoubles");
	res = table0->selectDoubles(colname, *hits);

	if (newlock) {
	    delete dslock;
	    dslock = 0;
	}
	if (ibis::gVerbose > 2)
	    logMessage("getQualifiedDoubles", "got %lu double value(s)",
		       static_cast<long unsigned>(res!=0 ? res->size() : 0));
    }
    return res;
} // ibis::query::getQualifiedDoubles

ibis::query::QUERY_STATE ibis::query::getState() const {
    if (ibis::gVerbose > 6) {
	switch (state) {
	case UNINITIALIZED:
	    logMessage("getState", "UNINITIALIZED"); break;
	case SET_RIDS:
	    logMessage("getState", "SET_RIDS"); break;
	case SET_COMPONENTS:
	    logMessage("getState", "SET_COMPONENTS"); break;
	case SET_PREDICATE:
	    logMessage("getState", "SET_PREDICATE"); break;
	case SPECIFIED:
	    logMessage("getState", "SPECIFIED"); break;
	case QUICK_ESTIMATE:
	    logMessage("getState", "QUICK_ESTIMATE"); break;
	case FULL_EVALUATE:
	    logMessage("getState", "FULL_EVALUATE"); break;
	default:
	    logMessage("getState", "UNKNOWN");
	}
    }
    return state;
} // ibis::query::getState

// expand predicate clause so that the conditions are all on preferred
// bounds
void ibis::query::expandQuery() {
    if (expr == 0) // no predicate clause specified
	return;

    writeLock lck(this, "expandQuery");
    if (dslock == 0) {
	dslock = new ibis::part::readLock(table0, myID);
    }
    doExpand(expr); // do the actual work

    // rewrite the query expression string
    std::ostringstream ostr;
    ostr << *expr;
    delete [] condition;
    condition = new char[ostr.str().size()+1];
    if (condition)
	strcpy(condition, ostr.str().c_str());

    // update the state of this query
    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	if (hits == sup) {
	    delete hits; hits = 0; sup = 0;
	}
	else {
	    delete hits; hits = 0;
	    delete sup; sup = 0;
	}
	state = SPECIFIED;
	removeFiles();
	dstime = 0;
    }
    else if (comps.size()) {
	state = SPECIFIED;
	writeQuery();
    }
} // ibis::query::expandQuery

// contract predicate clause so that the conditions are all on preferred
// bounds
void ibis::query::contractQuery() {
    if (expr == 0) // no predicate clause specified
	return;

    writeLock lck(this, "contractQuery");
    if (dslock == 0) {
	dslock = new ibis::part::readLock(table0, myID);
    }
    doContract(expr); // do the actual work

    // rewrite the query expression string
    std::ostringstream ostr;
    ostr << *expr;
    delete [] condition;
    condition = new char[ostr.str().size()+1];
    if (condition)
	strcpy(condition, ostr.str().c_str());

    // update the state of this query
    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	if (hits == sup) {
	    delete hits; hits = 0; sup = 0;
	}
	else {
	    delete hits; hits = 0;
	    delete sup; sup = 0;
	}
	state = SPECIFIED;
	removeFiles();
	dstime = 0;
    }
    else if (comps.size()) {
	state = SPECIFIED;
	writeQuery();
    }
} // ibis::query::contractQuery

/// Separate the simple range conditions from the more complex ones.  Reset
/// the where clause to the contain only the simple conditions.  The more
/// complex conditions are return in a string.
std::string ibis::query::removeComplexConditions() {
    std::string ret;
    if (expr == 0) return ret;

    ibis::qExpr *simple, *tail;
    int ierr = expr->separateSimple(simple, tail);
    if (ierr == 0) { // a mixture of complex and simple conditions
	QUERY_STATE old = state;
	std::ostringstream oss0, oss1;
	simple->print(oss0);
	tail->print(oss1);
	LOGGER(ibis::gVerbose >= 3) << "ibis::query::removeComplexConditions split \""
		  << (condition ? condition : "<long expression>")
		  << "\" into \"" << *simple << "\" ("
		  << oss0.str() << ") AND \"" << *tail << "\" ("
		  << oss1.str() << ")";

	delete simple;
	delete tail;
	ret = oss1.str();
	setWhereClause(oss0.str().c_str());
	if (old == QUICK_ESTIMATE)
	    estimate();
	else if (old == FULL_EVALUATE)
	    evaluate();
    }
    else if (ierr < 0) { // only complex conditions
	if (ibis::gVerbose > 2)
	    logMessage("removeComplexConditions", "the whole WHERE clause "
		       "is considered complex, no simple conjunctive "
		       "range conditions can be separated out");
	if (condition != 0) {
	    ret = condition;
	    delete [] condition;
	    condition = 0;
	}
	else {
	    std::ostringstream ostr;
	    ostr << *expr;
	    ret = ostr.str();
	}
	delete expr;
	expr = 0;
	if (rids_in == 0) {
	    if (sup != 0 && sup != hits) {
		delete sup;
		sup = 0;
	    }
	    if (hits == 0)
		hits = new ibis::bitvector;
	    hits->set(1, table0->nRows());
	    state = FULL_EVALUATE;
	}
	else if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
		 state == HITS_TRUNCATED || state == QUICK_ESTIMATE) {
	    getBounds();
	}
    }
    // ierr > 0 indicates that there are only simple conditions, do nothing
    return ret;
} // ibis::query::removeComplexConditions

////////////////////////////////////////////////////////////////////////////
//	The following functions are used internally by the query class
////////////////////////////////////////////////////////////////////////////
/// Construct a query object from scratch.  If recovery is desired or the
/// query objects has its own special prefix, a cache directory is created
/// to store some information about the query such as the query conditions
/// and the resulting solutions.  The stored information enables it to be
/// reconstructed in case of crash.
ibis::query::query(const char* uid, const part* et, const char* pref) :
    user(ibis::util::strnewdup(uid ? uid : ibis::util::userName())),
    condition(0), state(UNINITIALIZED), hits(0), sup(0), dslock(0),
    myID(0), myDir(0), rids_in(0), expr(0), table0(et), dstime(0) {
    myID = newToken(uid);
    lastError[0] = static_cast<char>(0);

    if (pthread_rwlock_init(&lock, 0) != 0) {
	strcpy(lastError, "pthread_rwlock_init() failed in "
	       "ibis::query::query()");
	LOGGER(ibis::gVerbose >= 0) << "Warning -- " << lastError;
	throw ibis::util::strnewdup(lastError);
    }

    std::string name;
    if (pref) {
	name = pref;
	name += ".enableRecovery";
    }
    else {
	name = "enableRecovery";
    }
    if (pref != 0 || ibis::gParameters().isTrue(name.c_str())) {
	setMyDir(pref);
    }
} // constructor for new query

/// Construct a query from the content stored in the named directory.  It
/// is used to recover a query from crash, not intended for user to
/// manually construct a query in a directory.
ibis::query::query(const char* dir, const ibis::partList& tl) :
    user(0), condition(0), state(UNINITIALIZED), hits(0), sup(0), dslock(0),
    myID(0), myDir(0), rids_in(0), expr(0), table0(0), dstime(0) {
    const char *ptr = strrchr(dir, DIRSEP);
    if (ptr == 0) {
	myID = ibis::util::strnewdup(dir);
	myDir = new char[strlen(dir)+2];
	strcpy(myDir, dir);
    }
    else if (ptr[1] == static_cast<char>(0)) { // dir name ends with DIRSEP
	myDir = ibis::util::strnewdup(dir);
	myDir[ptr-dir] = static_cast<char>(0);
	ptr = strrchr(myDir, DIRSEP);
	if (ptr != 0) {
	    myID = ibis::util::strnewdup(ptr+1);
	}
	else {
	    myID = ibis::util::strnewdup(myDir);
	}
    }
    else { 
	myID = ibis::util::strnewdup(ptr+1);
	myDir = new char[strlen(dir)+2];
	strcpy(myDir, dir);
    }
    uint32_t j = strlen(myDir);
    myDir[j] = DIRSEP;
    ++j;
    myDir[j] = static_cast<char>(0);

    readQuery(tl); // the directory must contain as least a query file
    if (state == QUICK_ESTIMATE) {
	state = SPECIFIED;
    }
    else if (state == FULL_EVALUATE) {
	try { // read the hit vector
	    readHits();
	    state = FULL_EVALUATE;
	}
	catch (...) { // failed to read the hit vector
	    if (hits != 0) delete hits;
	    hits = 0;
	    sup = 0;
	    if (comps.size() && (expr || rids_in))
		state = SPECIFIED;
	    else if (comps.size())
		state = SET_COMPONENTS;
	    else if (expr)
		state = SET_PREDICATE;
	    else if (rids_in)
		state = SET_RIDS;
	    else
		state = UNINITIALIZED;
	}
    }
} // constructor from stored files

// desctructor
ibis::query::~query() {
    clear();
    delete [] myDir;
    delete [] myID;
    delete [] user;
    pthread_rwlock_destroy(&lock);
}

/// To generate a new query token.  A token contains 16 bytes.  These bytes
/// are a base-64 representation of three integers computed from (A) the
/// Fletcher chechsum of the user id, (B) the current time stamp reported
/// by the function @c time, and (C) a monotonically increasing counter
/// provided by the function ibis::util::uniqueNumber.
char* ibis::query::newToken(const char *uid) {
    uint32_t ta, tb, tc;
    char* name = new char[ibis::query::tokenLength()+1];
    name[ibis::query::tokenLength()] = 0;

    // compute the three components of the token
    if (uid != 0 && *uid != 0)
	// a checksum of the user name
	ta = ibis::util::checksum(uid, strlen(uid));
    else
	ta = 0;
#if (_XOPEN_SOURCE - 0) >= 500
    ta ^= gethostid();			// the hostid (defined in unistd.h)
#endif
    {
	time_t tmp;
	time(&tmp);			// the current time
	tb = static_cast<uint32_t>(tmp);
    }
    tc = ibis::util::uniqueNumber();	// a counter

    if (ibis::gVerbose > 6)
	ibis::util::logMessage("newToken", "constructing token from "
			       "uid %s (%lu), time %lu, sequence "
			       "number %lu", uid,
			       static_cast<long unsigned>(ta),
			       static_cast<long unsigned>(tb),
			       static_cast<long unsigned>(tc));

    // write out the three integers as 16 printable characters
    name[15] = ibis::util::charTable[63 & tc]; tc >>= 6;
    name[14] = ibis::util::charTable[63 & tc]; tc >>= 6;
    name[13] = ibis::util::charTable[63 & tc]; tc >>= 6;
    name[12] = ibis::util::charTable[63 & tc]; tc >>= 6;
    name[11] = ibis::util::charTable[63 & tc]; tc >>= 6;
    name[10] = ibis::util::charTable[63 & (tc | (tb<<2))]; tb >>= 4;
    name[9]  = ibis::util::charTable[63 & tb]; tb >>= 6;
    name[8]  = ibis::util::charTable[63 & tb]; tb >>= 6;
    name[7]  = ibis::util::charTable[63 & tb]; tb >>= 6;
    name[6]  = ibis::util::charTable[63 & tb]; tb >>= 6;
    name[5]  = ibis::util::charTable[63 & (tb | (ta<<4))]; ta >>= 2;
    name[4]  = ibis::util::charTable[63 & ta]; ta >>= 6;
    name[3]  = ibis::util::charTable[63 & ta]; ta >>= 6;
    name[2]  = ibis::util::charTable[63 & ta]; ta >>= 6;
    name[1]  = ibis::util::charTable[63 & ta]; ta >>= 6;
    // ensure the first byte is one of alphabets
    if (ta > 9 && ta < 62) {
	name[0]  = ibis::util::charTable[ta];
    }
    else {
	// attempt to use the first alphabet of uid
	const char *tmp = uid;
	if (uid != 0 && *uid != 0)
	    while (*tmp && !isalpha(*tmp)) ++ tmp;
	if (tmp != 0 && *tmp != 0) { // found an alphabet
	    name[0] = *tmp;
	}
	else if (ta <= 9) {
	    name[0] = ibis::util::charTable[ta*5+10];
	}
	else {
	    ta -= 62;
	    ta &= 31; // possible values [0:31]
	    name[0] = ibis::util::charTable[ta+10];
	}
    }
    if (ibis::gVerbose > 3)
        ibis::util::logMessage("newToken", "generated new token \"%s\" "
			       "for user %s", name, uid);
    return name;
} // ibis::query::newToken

// is the given string a valid query token
// -- must have 16 characters
// -- must be all in charTable
bool ibis::query::isValidToken(const char* tok) {
    bool ret = (strlen(tok) == ibis::query::tokenLength());
    if (! ret) // if string length not 16, it can not be a valid token
	return ret;
    // necessary to prevent overstepping the bouds of array
    // ibis::util::charIndex
    ret = (tok[0] < 127) && (tok[1] < 127) && (tok[2] < 127) &&
	(tok[3] < 127) && (tok[4] < 127) && (tok[5] < 127) &&
	(tok[6] < 127) && (tok[7] < 127) && (tok[8] < 127) &&
	(tok[9] < 127) && (tok[10] < 127) && (tok[11] < 127) &&
	(tok[12] < 127) && (tok[13] < 127) && (tok[14] < 127) &&
	(tok[15] < 127);
    if (! ret)
	return ret;

    // convert 16 character to 3 integers
    uint32_t ta, tb, tc, tmp;
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[0])];
    if (tmp < 64) {
	ta = (tmp << 26);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[1])];
    if (tmp < 64) {
	ta |= (tmp << 20);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[2])];
    if (tmp < 64) {
	ta |= (tmp << 14);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[3])];
    if (tmp < 64) {
	ta |= (tmp << 8);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[4])];
    if (tmp < 64) {
	ta |= (tmp << 2);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[5])];
    if (tmp < 64) {
	ta |= (tmp >> 4);
	tb = (tmp << 28);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[6])];
    if (tmp < 64) {
	tb |= (tmp << 22);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[7])];
    if (tmp < 64) {
	tb |= (tmp << 16);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[8])];
    if (tmp < 64) {
	tb |= (tmp << 10);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[9])];
    if (tmp < 64) {
	tb |= (tmp << 4);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[10])];
    if (tmp < 64) {
	tb |= (tmp >> 2);
	tc = (tmp << 30);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[11])];
    if (tmp < 64) {
	tc |= (tmp << 24);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[12])];
    if (tmp < 64) {
	tc |= (tmp << 18);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[13])];
    if (tmp < 64) {
	tc |= (tmp << 12);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[14])];
    if (tmp < 64) {
	tc |= (tmp << 6);
    }
    else {
	ret = false;
	return ret;
    }
    tmp = ibis::util::charIndex[static_cast<unsigned>(tok[15])];
    if (tmp < 64) {
	tc |= tmp;
    }
    else {
	ret = false;
	return ret;
    }

    if (ibis::gVerbose > 8)
	ibis::util::logMessage("isValidToken", "convert token %s to three "
			       "integers %lu, %lu, %lu.",
			       tok, static_cast<long unsigned>(ta),
			       static_cast<long unsigned>(tb),
			       static_cast<long unsigned>(tc));

    long unsigned tm; // current time in seconds
    (void) time((time_t*)&tm);
    ret = (tm >= tb); // must be created in the past

    return ret;
} // ibis::query::isValidToken

/// To determine an directory for storing information about the query, such
/// as the where clause, the hits and so on.  It can also be used to
/// recover from a crash.
void ibis::query::setMyDir(const char *pref) {
    if (myDir != 0) return; // do not over write existing value

    const char* cacheDir = 0;
    if (pref == 0 || *pref == 0) {
	cacheDir = ibis::gParameters()["CacheDirectory"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["CacheDir"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["query.CacheDirectory"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["query.CacheDir"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["query.dataDir3"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["ibis.query.CacheDirectory"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["ibis.query.CacheDir"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["ibis.query.dataDir3"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()
		["GCA.coordinator.cacheDirectory"];
	if (cacheDir == 0)
	    cacheDir = ibis::gParameters()["GCA.coordinator.cacheDir"];
    }
    else {
	std::string name = pref;
	name += ".cacheDirectory";
	cacheDir = ibis::gParameters()[name.c_str()];
	if (cacheDir == 0) {
	    name = pref;
	    name += ".cacheDir";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = pref;
	    name += ".dataDir3";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = pref;
	    name += ".query.cacheDirectory";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = pref;
	    name += ".query.cacheDir";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = pref;
	    name += ".query.dataDir3";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = "ibis.";
	    name += pref;
	    name += ".query.cacheDirectory";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = "ibis.";
	    name += pref;
	    name += ".query.cacheDir";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = "ibis.";
	    name += pref;
	    name += ".query.dataDir3";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = "GCA.";
	    name += pref;
	    name += ".coordinator.cacheDirectory";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
	if (cacheDir == 0) {
	    name = "GCA.";
	    name += pref;
	    name += ".coordinator.cacheDir";
	    cacheDir = ibis::gParameters()[name.c_str()];
	}
    }
#if defined(unix)
    if (cacheDir == 0) {
	cacheDir = getenv("TMPDIR");
    }
#endif

    if (cacheDir) {
	if (strlen(cacheDir)+strlen(myID)+10<PATH_MAX) {
	    myDir = new char[strlen(cacheDir)+strlen(myID)+3];
	    sprintf(myDir, "%s%c%s", cacheDir, DIRSEP, myID);
	}
	else {
	    LOGGER(ibis::gVerbose >= 0)
		<< "Warning -- CacheDirectory(\"" << cacheDir
		<< "\") too long";
	    throw "path for CacheDirectory is too long";
	}
    }
    else {
	myDir = new char[10+strlen(myID)];
	sprintf(myDir, ".ibis%c%s", DIRSEP, myID);
    }
    uint32_t j = strlen(myDir);
    myDir[j] = DIRSEP;
    myDir[j+1] = static_cast<char>(0);
    ibis::util::makeDir(myDir);
} /// ibis::query::setMyDir

/// This function prints a list of RIDs to the log file.
void ibis::query::printRIDs(const ibis::RIDSet& ridset) const {
    if (ibis::gVerbose < 0) return;

    int len = ridset.size();
    ibis::util::logger lg(4);
    ibis::RIDSet::const_iterator it = ridset.begin();
    lg.buffer() << "RID set length = " << len << std::endl;
    for (int i=0; i<len; ++i, ++it) {
	lg.buffer() << " [ " << (*it).num.run << ", "
		    << (*it).num.event << " ] ";
	if (3 == i%4)
	    lg.buffer() << std::endl;
    }
    if (len>0 && len%4!=0)
	lg.buffer() << std::endl;
} // ibis::query::printRIDs

// three error logging functions
void ibis::query::logError(const char* event, const char* fmt, ...) const {
    strcpy(lastError, "ERROR: ");

#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    char* s = new char[strlen(fmt)+MAX_LINE];
    if (s != 0) {
	va_list args;
	va_start(args, fmt);
	vsprintf(s, fmt, args);
	va_end(args);

	(void) strncpy(lastError+7, s, MAX_LINE-7);
	{
	    ibis::util::logger lg(ibis::gVerbose + 2);
	    lg.buffer() << " Error *** query[" << myID << "]::" << event
			<< " -- " << s;
	    if (errno != 0)
		lg.buffer() << " ... " << strerror(errno);
	}
	throw s;
    }
    else {
#endif
	(void) strncpy(lastError+7, fmt, MAX_LINE-7);
	{
	    ibis::util::logger lg(ibis::gVerbose + 2);
	    lg.buffer() << " Error *** query[" << myID << "]::" << event
			<< " -- " << fmt << " ...";
	    if (errno != 0)
		lg.buffer() << " ... " << strerror(errno);
	}
	throw fmt;
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    }
#endif
} // ibis::query::logError

void ibis::query::logWarning(const char* event, const char* fmt, ...) const {
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    if (strnicmp(lastError, "ERROR", 5) != 0) {
	// last message was not an error, record this warning message
	strcpy(lastError, "Warning: ");
	va_list args;
	va_start(args, fmt);
	vsprintf(lastError+9, fmt, args);
	va_end(args);

	ibis::util::logger lg(ibis::gVerbose+1);
	lg.buffer() << "Warning -- query[" << myID << "]::"
		    << event << " -- " << lastError+9;
	if (errno != 0) {
	    if (errno != ENOENT)
		lg.buffer() << " ... " << strerror(errno);
	    errno = 0;
	}
    }
    else {
	char* s = new char[strlen(fmt)+MAX_LINE];
	if (s != 0) {
	    va_list args;
	    va_start(args, fmt);
	    vsprintf(s, fmt, args);
	    va_end(args);

	    ibis::util::logger lg(ibis::gVerbose+1);
	    lg.buffer() << "Warning -- query[" << myID << "]::" << event
			<< " -- " << s;
	    if (errno != 0) {
		if (errno != ENOENT)
		    lg.buffer() << " ... " << strerror(errno);
		errno = 0;
	    }
	    delete [] s;
	}
	else {
	    FILE* fptr = ibis::util::getLogFile();
	    ibis::util::ioLock lock;
	    fprintf(fptr, "Warning -- query[%s]::%s -- ", myID, event);
	    va_list args;
	    va_start(args, fmt);
	    vfprintf(fptr, fmt, args);
	    va_end(args);
	    fprintf(fptr, "\n");
	    fflush(fptr);
	}
    }
#else
    if (strnicmp(lastError, "ERROR", 5) != 0) {
	UnixSnprintf(lastError, MAX_LINE+PATH_MAX, "Warning: %s", fmt);
    }

    ibis::util::logger lg(ibis::gVerbose+1);
    lg.buffer() << "Warning -- query[" << myID << "]::" << event
		<< " -- " << fmt << " ..."
    if (errno != 0) {
	if (errno != ENOENT)
	    lg.buffer() << " ... " << strerror(errno);
	errno = 0;
    }
#endif
} // ibis::query::logWarning

void ibis::query::logMessage(const char* event, const char* fmt, ...) const {
    FILE *fptr = ibis::util::getLogFile();
    ibis::util::ioLock lck;
#if defined(TIMED_LOG)
    char tstr[28];
    ibis::util::getLocalTime(tstr);
    fprintf(fptr, "%s   ", tstr);
#endif
    fprintf(fptr, "query[%s]::%s -- ", myID, event);
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    va_list args;
    va_start(args, fmt);
    vfprintf(fptr, fmt, args);
    va_end(args);
#else
    fprintf(fptr, "%s ...", fmt);
#endif
    fprintf(fptr, "\n");
    fflush(fptr);
} // ibis::query::logMessage

// verify that the names are in the current list of attributes
int ibis::query::verifyPredicate(ibis::qExpr*& qexpr) {
    int ierr = 0;

    if (qexpr == 0 || table0 == 0)
	return ierr;

    switch (qexpr->getType()) {
    case ibis::qExpr::RANGE: {
	ibis::qContinuousRange* range =
	    static_cast<ibis::qContinuousRange*>(qexpr);
	if (range->colName()) { // allow name to be NULL
	    const ibis::column* col = table0->getColumn(range->colName());
	    if (col == 0) {
		++ ierr;
		logWarning("verifyPredicate", "table %s does not "
			   "contain a column named %s", table0->name(),
			   range->colName());
	    }
	    else if (col->type() == ibis::FLOAT) {
		// reduce the precision of the bounds
		range->leftBound() =
		    static_cast<float>(range->leftBound());
		range->rightBound() =
		    static_cast<float>(range->rightBound());
	    }
	}
	break;}
    case ibis::qExpr::STRING: {
	const ibis::qString* str =
	    static_cast<const ibis::qString*>(qexpr);
	if (str->leftString()) { // allow name to be NULL
	    const ibis::column* col =
		table0->getColumn(str->leftString());
	    if (col == 0) {
		++ ierr;
		logWarning("verifyPredicate", "table %s does not "
			   "contain a column named %s", table0->name(),
			   str->leftString());
	    }
	}
	break;}
    case ibis::qExpr::MATHTERM: {
	ibis::compRange::term* math =
	    static_cast<ibis::compRange::term*>(qexpr);
	if (math->termType() == ibis::compRange::VARIABLE) {
	    const ibis::compRange::variable* var =
		static_cast<const ibis::compRange::variable*>(math);
	    const ibis::column* col =
		table0->getColumn(var->variableName());
	    if (col == 0) {
		++ ierr;
		logWarning("verifyPredicate", "table %s does not "
			   "contain a column named %s", table0->name(),
			   var->variableName());
	    }
	}
	ibis::qExpr *tmp = math->getLeft();
	if (tmp != 0)
	    ierr += verifyPredicate(tmp);
	tmp = math->getRight();
	if (tmp != 0)
	    ierr += verifyPredicate(tmp);
	break;}
    case ibis::qExpr::COMPRANGE: {
	// compRange have three terms rather than two
	if (reinterpret_cast<ibis::compRange*>(qexpr)
	    ->maybeStringCompare()) {
	    const ibis::compRange::variable *v1 =
		reinterpret_cast<const ibis::compRange::variable*>
		(qexpr->getLeft());
	    const ibis::compRange::variable *v2 =
		reinterpret_cast<const ibis::compRange::variable*>
		(qexpr->getRight());
	    const ibis::column *c1 =
		table0->getColumn(v1->variableName());
	    const ibis::column *c2 =
		table0->getColumn(v2->variableName());
	    if (c1 != 0) {
		if (c2 == 0) {
		    if (c1->type() == ibis::TEXT ||
			c1->type() == ibis::CATEGORY) {
			if (ibis::gVerbose > 3)
			    logMessage("verifyPredicate", "replacing (%s = "
				       "%s) with (%s = \"%s\")",
				       v1->variableName(),
				       v2->variableName(),
				       v1->variableName(),
				       v2->variableName());
			ibis::qString *tmp = new
			    ibis::qString(v1->variableName(),
					  v2->variableName());
			delete qexpr;
			qexpr = tmp;
		    }
		    else {
			++ ierr;
			logWarning("verifyPredicate", "expected column "
				   "\"%s\" to be of string type, but "
				   "it is %s", v1->variableName(),
				   ibis::TYPESTRING[c1->type()]);
		    }
		}
	    }
	    else if (c2 != 0) {
		if (c2->type() == ibis::TEXT ||
		    c2->type() == ibis::CATEGORY) {
		    if (ibis::gVerbose > 3)
			logMessage("verifyPredicate", "replacing (%s = %s) "
				   "with (%s = \"%s\")",v2->variableName(),
				   v1->variableName(), v2->variableName(),
				   v1->variableName());
		    ibis::qString *tmp = new
			ibis::qString(v2->variableName(),
				      v1->variableName());
		    delete qexpr;
		    qexpr = tmp;
		}
		else {
		    ++ ierr;
		    logWarning("verifyPredicate", "expected column "
			       "\"%s\" to be of string type, but "
			       "it is %s", v2->variableName(),
			       ibis::TYPESTRING[c2->type()]);
		}
	    }
	    else {
		ierr += 2;
		logWarning("verifyPredicate", "neither %s or %s are "
			   "columns names of table %s",
			   v1->variableName(), v2->variableName(),
			   table0->name());
	    }
	}
	else {
	    if (qexpr->getLeft() != 0)
		ierr = verifyPredicate(qexpr->getLeft());
	    if (qexpr->getRight() != 0)
		ierr += verifyPredicate(qexpr->getRight());
	    ibis::qExpr* cr = reinterpret_cast<ibis::compRange*>
		(qexpr)->getTerm3();
	    ierr += verifyPredicate(cr);
	}
	break;}
    case ibis::qExpr::DRANGE : {
	ibis::qDiscreteRange *range =
	    reinterpret_cast<ibis::qDiscreteRange*>(qexpr);
	if (range->colName()) { // allow name to be NULL
	    const ibis::column* col = table0->getColumn(range->colName());
	    if (col == 0) {
		++ ierr;
		logWarning("verifyPredicate", "table %s does not "
			   "contain a column named %s", table0->name(),
			   range->colName());
	    }
	    else if (col->type() == ibis::FLOAT) {
		// reduce the precision of the bounds
		std::vector<double>& val = range->getValues();
		for (std::vector<double>::iterator it = val.begin();
		     it != val.end(); ++ it)
		    *it = static_cast<float>(*it);
	    }
	}
	break;}
    case ibis::qExpr::MSTRING : {
	ibis::qMultiString *range =
	    reinterpret_cast<ibis::qMultiString*>(qexpr);
	if (range->colName()) { // allow name to be NULL
	    const ibis::column* col = table0->getColumn(range->colName());
	    if (col == 0) {
		++ ierr;
		logWarning("verifyPredicate", "table %s does not "
			   "contain a column named %s", table0->name(),
			   range->colName());
	    }
	}
	break;}
    case ibis::qExpr::JOIN : {
	ibis::rangeJoin *rj = reinterpret_cast<ibis::rangeJoin*>(qexpr);
	const ibis::column* c1 = table0->getColumn(rj->getName1());
	if (c1 == 0) {
	    ++ ierr;
	    logWarning("verifyPredicate", "table %s does not "
		       "contain a column named %s", table0->name(),
		       rj->getName1());
	}
	const ibis::column* c2 = table0->getColumn(rj->getName2());
	if (c2 == 0) {
	    ++ ierr;
	    logWarning("verifyPredicate", "table %s does not "
		       "contain a column named %s", table0->name(),
		       rj->getName2());
	}
	ibis::qExpr *t = rj->getRange();
	ierr += verifyPredicate(t);
	break;}
    default: {
	if (qexpr->getLeft() != 0)
	    ierr = verifyPredicate(qexpr->getLeft());
	if (qexpr->getRight() != 0)
	    ierr += verifyPredicate(qexpr->getRight());
	break;}
    } // end switch

    return ierr;
} // ibis::query::verifyPredicate

bool ibis::query::hasBundles() const {
    char ridfile[PATH_MAX];
    char bdlfile[PATH_MAX];
    strcpy(ridfile, dir());
    strcpy(bdlfile, dir());
    strcat(ridfile, "rids");
    strcat(bdlfile, "bundles");
    if (ibis::util::getFileSize(ridfile) > 0 &&
	ibis::util::getFileSize(bdlfile) > 0) {
	return true;
    }
    else {
	return false;
    }
} // ibis::query::hasBundles()

// reorder the query expression to minimize the work of evaluation
// (assuming the query expression is evaluated from left to right)
void ibis::query::reorderExpr() {
    ibis::query::weight wt(table0);

    // call qExpr::reorder to do the actual work
    double ret = expr->reorder(wt);
    LOGGER(ibis::gVerbose >= 6) << "query[" << myID << "]:reorderExpr returns " << ret
	      << ".  The new query expression is \n" << *expr;
} // ibis::query::reorderExpr

void ibis::query::getBounds() {
    if (ibis::gVerbose > 7)
	logMessage("getBounds", "compute upper and lower bounds of hits");

    ibis::bitvector mask(table0->getMask());
    if (mask.size() != table0->nRows())
	mask.adjustSize(table0->nRows(), table0->nRows());
    if (comps.size() > 0) {
	ibis::bitvector tmp;
	ibis::selected::const_iterator it = comps.begin();
	while (it != comps.end()) {
	    table0->getColumn(it->c_str())->getNullMask(tmp);
	    mask &= tmp;
#if defined(DEBUG)
	    LOGGER(ibis::gVerbose >= 0) << *it << " null mask:\n" << tmp
				   << "\nquery mask:\n" << mask;
#endif
	    ++ it;
	}
    }
    else if (ibis::gVerbose > 3) {
	logMessage("getBounds", "no component selected");
    }

    if (rids_in) { // RID list
	ibis::bitvector tmp;
	table0->evaluateRIDSet(*rids_in, tmp);
	mask &= tmp;
    }

    if (expr) { // range condition
	sup = new ibis::bitvector;
	hits = new ibis::bitvector;
	doEstimate(expr, *hits, *sup);
	if (hits->size() != table0->nRows()) {
	    logWarning("getBounds", "hits.size(%lu) differ from expected "
		       "value(%lu)", static_cast<long unsigned>(hits->size()),
		       static_cast<long unsigned>(table0->nRows()));
	    hits->setBit(table0->nRows()-1, 0);
	}
	*hits &= mask;
	hits->compress();

	if (sup->size() != table0->nRows() && sup->size() > 0) {
	    if (sup->size() && ibis::gVerbose > 3)
		logMessage("getBounds", "sup.size(%lu) differ from expected "
			   "value(%lu)",
			   static_cast<long unsigned>(sup->size()),
			   static_cast<long unsigned>(table0->nRows()));
	    sup->clear(); // assume hits is accruate
	}
	if (sup->size() == hits->size()) {
	    *sup &= mask;
	    sup->compress();
	    if (ibis::gVerbose > 3)
		logMessage("getBounds", "number of hits in [%lu, %lu]",
			   static_cast<long unsigned>(hits->cnt()),
			   static_cast<long unsigned>(sup->cnt()));
	}
	else {
	    delete sup;
	    sup = hits;
	}
    }
    else { // everything is a hit
	hits = new ibis::bitvector(mask);
	sup = hits;
    }
} // ibis::query::getBounds

// perform quick estimation only
void ibis::query::doEstimate(const ibis::qExpr* term, ibis::bitvector& low,
			     ibis::bitvector& high) const {
    if (term == 0) return;
    LOGGER(ibis::gVerbose >= 8) << "query[" << myID
	      << "]::doEstimate -- starting to estimate " << *term;

    switch (term->getType()) {
    case ibis::qExpr::LOGICAL_NOT: {
	doEstimate(term->getLeft(), high, low);
	high.flip();
	if (low.size() == high.size()) {
	    low.flip();
	}
	else {
	    low.swap(high);
	}
	break;
    }
    case ibis::qExpr::LOGICAL_AND: {
	doEstimate(term->getLeft(), low, high);
	// there is no need to evaluate the right-hand side if the left-hand
	// is evaluated to have no hit
	if (low.cnt() > 0 || (high.size() == low.size() && high.cnt() > 0)) {
	    // continue to evaluate the right-hand side
	    ibis::bitvector b1, b2;
	    doEstimate(term->getRight(), b1, b2);
	    if (high.size() == low.size()) {
		if (b2.size() == b1.size()) {
		    high &= b2;
		}
		else {
		    high &= b1;
		}
	    }
	    else if (b2.size() == b1.size()) {
		high.copy(low);
		high &= b2;
	    }
	    low &= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_OR: {
	ibis::bitvector b1, b2;
	doEstimate(term->getLeft(), low, high);
	doEstimate(term->getRight(), b1, b2);
	if (high.size() == low.size()) {
	    if (b2.size() == b1.size()) {
		high |= b2;
	    }
	    else {
		high |= b1;
	    }
	}
	else if (b2.size() == b1.size()) {
	    high.copy(low);
	    high |= b2;
	}
	low |= b1;
	break;
    }
    case ibis::qExpr::LOGICAL_XOR: {
	// based on the fact that a ^ b = a - b | b - a
	// the lower and upper bounds can be computed as two separated
	// quantities
	// the whole process generates 10 new bit vectors and explicitly
	// destroys 6 of them, returns two to the caller and implicitly
	// destroys 2 (b1, b2)
	ibis::bitvector b1, b2;
	ibis::bitvector *b3, *b4, *b5;
	doEstimate(term->getLeft(), b1, b2);
	doEstimate(term->getRight(), low, high);
	if (high.size() == low.size()) {
	    if (b1.size() == b2.size()) {
		b3 = b1 - high;
		b4 = low - b2;
		b5 = *b3 | *b4;
		low.swap(*b5);
		delete b3;
		delete b4;
		b3 = high - b1;
		b4 = b2 - *b5;
		delete b5;
		b5 = *b3 | *b4;
		high.swap(*b5);
		delete b5;
		delete b4;
		delete b3;
	    }
	    else {
		b3 = b1 - high;
		b4 = low - b1;
		b5 = *b3 | *b4;
		low.swap(*b5);
		delete b3;
		delete b4;
		b3 = high - b1;
		b4 = b1 - *b5;
		delete b5;
		b5 = *b3 | *b4;
		high.swap(*b5);
		delete b5;
		delete b4;
		delete b3;
	    }
	}
	else if (b1.size() == b2.size()) {
	    b3 = b1 - low;
	    b4 = low - b2;
	    b5 = *b3 | *b4;
	    low.swap(*b5);
	    delete b3;
	    delete b4;
	    b3 = low - b1;
	    b4 = b2 - *b5;
	    delete b5;
	    b5 = *b3 | *b4;
	    high.swap(*b5);
	    delete b5;
	    delete b4;
	    delete b3;
	}
	else {
	    low ^= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_MINUS: {
	doEstimate(term->getLeft(), low, high);
	// there is no need to evaluate the right-hand side if the left-hand
	// is evaluated to have no hit
	if (low.cnt() > 0 || (high.size() == low.size() && high.cnt() > 0)) {
	    // continue to evaluate the right-hand side
	    ibis::bitvector b1, b2;
	    doEstimate(term->getRight(), b2, b1);
	    if (high.size() == low.size()) {
		if (b1.size() == b2.size()) {
		    high -= b2;
		    low -= b1;
		}
		else {
		    high -= b2;
		    low -= b2;
		}
	    }
	    else if (b1.size() == b2.size()) {
		high.copy(low);
		high -= b2;
		low -= b1;
	    }
	    else {
		low -= b2;
	    }
	}
	break;
    }
    case ibis::qExpr::RANGE:
	table0->estimateRange
	    (*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
	     low, high);
	break;
    case ibis::qExpr::DRANGE:
	table0->estimateRange
	    (*(reinterpret_cast<const ibis::qDiscreteRange*>(term)),
	     low, high);
	break;
    case ibis::qExpr::STRING:
	table0->lookforString
	    (*(reinterpret_cast<const ibis::qString*>(term)), low);
	high.clear();
	break;
    case ibis::qExpr::MSTRING:
	table0->lookforString
	    (*(reinterpret_cast<const ibis::qMultiString*>(term)), low);
	high.clear();
	break;
    case ibis::qExpr::ANYANY:
	table0->estimateMatchAny
	    (*(reinterpret_cast<const ibis::qAnyAny*>(term)), low, high);
	break;
    case ibis::qExpr::COMPRANGE:
	// can not estimate complex range condition yet
	high.set(1, table0->nRows());
	low.set(0, table0->nRows());
	break;
    default:
	if (ibis::gVerbose > 2)
	    logMessage("doEstimate", "unable to estimate query term of "
		       "unknown type, presume every row is a hit");
	high.set(1, table0->nRows());
	low.set(1, table0->nRows());
    }
#ifdef DEBUG
    LOGGER(ibis::gVerbose >= 0)
	<< "ibis::query[" << myID << "]::doEstimate(" << *term
	<< ") --> [" << low.cnt() << ", " << high.cnt() << "]";
#if DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0) << "low \n" << low
			   << "\nhigh \n" << high;
#else
    if (ibis::gVerbose >= 30 ||
	((low.bytes()+high.bytes()) < (2U << ibis::gVerbose))) {
	LOGGER(ibis::gVerbose >= 0) << "low \n" << low
			       << "\nhigh \n" << high;
    }
#endif
#else
    LOGGER(ibis::gVerbose >= 5) << "ibis::query[" << myID << "]::doEstimate(" << *term
	      << ") --> [" << low.cnt() << ", "
	      << (high.size()==low.size() ? high.cnt() : low.cnt()) << "]";
#endif
} // ibis::query::doEstimate

int ibis::query::computeHits() {
    if (ibis::gVerbose > 7) {
	ibis::util::logger lg(7);
	lg.buffer() << "ibis::query[" << myID << "]::computeHits -- "
	    "starting to compute hits for the query";
	if (expr != 0)
	    lg.buffer() << " \""<< *expr << "\"";
    }

    int ierr = 0;
    if (hits == 0) { // have not performed an estimate
	if (rids_in) { // has a RID list
	    getBounds();
	}
	else if (expr) { // usual range query
	    dstime = table0->timestamp();
	    hits = new ibis::bitvector;
#ifndef DONOT_REORDER_EXPRESSION
	    if (! expr->directEval())
		reorderExpr();
#endif
	    delete sup;
	    sup = 0;
	    ierr = doEvaluate(expr, table0->getMask(), *hits);
	    if (ierr < 0)
		return ierr - 20;
	    hits->compress();
	    sup = hits;
	}
	else { // should not enter here, caller has checked expr and rids_in
	    logWarning("computeHits", "either a query condition or a RID "
		       "set must be specified.");
	    return -8;
	}
    }

    if (sup == 0) { // already have the exact answer
	sup = hits; // copy the pointer to make other operations easier
    }
    else if (hits->size() != sup->size() || hits->cnt() >= sup->cnt()) {
	// the estimate is accurate -- no need for actual scan
	if (sup != hits) {
	    delete sup;
	    sup = hits;
	}
    }
    else { // need to actually examine the data files involved
	const ibis::bitvector& msk = table0->getMask();
	ibis::bitvector delta;
	(*sup) -= (*hits);

	if (sup->cnt() < (msk.cnt() >> 2)) { // use doScan
	    ierr = doScan(expr, *sup, delta);
	    if (ierr >= 0) {
		delete sup;  // no longer need it
		*hits |= delta;
		sup = hits;
	    }
	    else {
		(*sup) |= (*hits);
		return ierr - 20;
	    }
	}
	else { // use doEvaluate
	    delete sup;
	    sup = 0;
	    ierr = doEvaluate(expr, msk, *hits);
	    if (ierr < 0)
		return ierr - 20;
	    hits->compress();
	    sup = hits;
	}
    }

    if ((expr == 0 || ! expr->hasJoin()) &&
	(ibis::gVerbose >= 30 || (ibis::gVerbose > 4 &&
				  (1U<<ibis::gVerbose) >= hits->bytes()))) {
	ibis::util::logger lg(4);
	lg.buffer() << "ibis::query::computeHits() hit vector" << *hits
		    << "\n";
	if (ibis::gVerbose > 19) {
	    ibis::bitvector::indexSet is = hits->firstIndexSet();
	    lg.buffer() << "row numbers of the hits\n";
	    while (is.nIndices()) {
		const ibis::bitvector::word_t *ii = is.indices();
		if (is.isRange()) {
		    lg.buffer() << *ii << " -- " << ii[1];
		}
		else {
		    for (unsigned i=0; i < is.nIndices(); ++ i)
			lg.buffer() << ii[i] << " ";
		}
		lg.buffer() << "\n";
		++ is;
	    }
	}
    }
    return ierr;
} // ibis::query::computeHits

// perform a simple sequential scan by using the function doScan
long ibis::query::sequentialScan(ibis::bitvector& res) const {
    if (expr == 0)
	return -8;

    long ierr;
    readLock lock(this, "sequentialScan"); // read lock on query
    ibis::part::readLock lds(table0, myID); // read lock on data
    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();
    try {
	ierr = doScan(expr, table0->getMask(), res);
	if (ierr < 0)
	    return ierr - 20;
    }
    catch (const ibis::bad_alloc& e) {
	ierr = -1;
	res.clear();
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- ibis::query[" << myID << "]::sequentialScan("
	    << *expr << ") failed due to a memory allocation problem, "
	    << e.what();
    }
    catch (const std::exception& e) {
	ierr = -2;
	res.clear();
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- ibis::query[" << myID << "]::sequentialScan("
	    << *expr << ") failed due to a std::exception, " << e.what();
    }
    catch (const char *e) {
	ierr = -3;
	res.clear();
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- ibis::query[" << myID << "]::sequentialScan("
	    << *expr << ") failed due to a string exception, " << e;
    }
    catch (...) {
	ierr = -4;
	res.clear();
	LOGGER(ibis::gVerbose >= 0)
	    << "Warning -- ibis::query[" << myID << "]::sequentialScan("
	    << *expr << ") failed due to an unexpected exception";
    }

    if (ierr > 0 && ibis::gVerbose > 2) {
	timer.stop();
	logMessage("sequentialScan", "produced %ld hit%s in %g sec(CPU) "
		   "%g sec(elapsed).", ierr, (ierr>1?"s":""),
		   timer.CPUTime(), timer.realTime());
	if (ibis::gVerbose > 4 && hits != 0 && state == FULL_EVALUATE) {
	    ibis::bitvector diff;
	    diff.copy(*hits);
	    diff ^= res;
	    if (diff.cnt()) {
		logWarning("sequentialScan", "produced %lu hit%s that "
			   "are different from the previous evaluation",
			   static_cast<long unsigned>(diff.cnt()),
			   (diff.cnt()>1?"s":""));
		if (ibis::gVerbose > 6) {
		    uint32_t maxcnt = (ibis::gVerbose > 30 ? table0->nRows()
				       : (1U << ibis::gVerbose));
		    if (maxcnt > diff.cnt())
			maxcnt = diff.cnt();
		    uint32_t cnt = 0;
		    ibis::bitvector::indexSet is = diff.firstIndexSet();

		    ibis::util::logger lg(2);
		    lg.buffer() << "row numbers of mismatching hits\n";
		    while (is.nIndices() && cnt < maxcnt) {
			const ibis::bitvector::word_t *ii = is.indices();
			if (is.isRange()) {
			    lg.buffer() << *ii << " -- " << ii[1];
			}
			else {
			    for (unsigned i=0; i < is.nIndices(); ++ i)
				lg.buffer() << ii[i] << " ";
			}
			cnt += is.nIndices();
			lg.buffer() << "\n";
			++ is;
		    }
		    if (cnt < diff.cnt())
			lg.buffer() << "... (" << diff.cnt() - cnt
				    << " rows skipped\n";
		}
	    }
	}
    }
    return ierr;
} // ibis::query::sequentialScan

long ibis::query::getExpandedHits(ibis::bitvector& res) const {
    long ierr;
    readLock lock(this, "getExpandedHits"); // don't change query
    if (expr != 0) {
	ibis::part::readLock lock2(table0, myID); // don't change data
	doEvaluate(expr, res);
	ierr = res.cnt();
    }
    else if (rids_in != 0) {
	ibis::part::readLock lock2(table0, myID);
	table0->evaluateRIDSet(*rids_in, res);
	ierr = res.cnt();
    }
    else {
	res.clear();
	ierr = -8;
    }
    return ierr;
} // ibis::query::getExpandedHits

// perform sequential scan
int ibis::query::doScan(const ibis::qExpr* term,
			ibis::bitvector& ht) const {
    int ierr = 0;
    if (term == 0) return ierr;
    if (term == 0) { // no hits
	ht.set(0, table0->nRows());
	return ierr;
    }
    LOGGER(ibis::gVerbose >= 8) << "query::[" << myID
	      << "]::doScan -- reading data entries to resolve " << *term;

    switch (term->getType()) {
    case ibis::qExpr::LOGICAL_NOT: {
	ierr = doScan(term->getLeft(), ht);
	if (ierr >= 0)
	    ht.flip();
	break;
    }
    case ibis::qExpr::LOGICAL_AND: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), b1);
	if (ierr >= 0)
	    ierr = doScan(term->getRight(), b1, ht);
	break;
    }
    case ibis::qExpr::LOGICAL_OR: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), ht);
	if (ierr >= 0) {
	    ierr = doScan(term->getRight(), b1);
	    if (ierr >= 0)
		ht |= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_XOR: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), ht);
	if (ierr >= 0) {
	    ierr = doScan(term->getRight(), b1);
	    if (ierr >= 0)
		ht ^= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_MINUS: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), ht);
	if (ierr >= 0) {
	    ierr = doScan(term->getRight(), ht, b1);
	    if (ierr >= 0)
		ht -= b1;
	}
	break;
    }
    case ibis::qExpr::RANGE:
	ierr = table0->doScan
	    (*(reinterpret_cast<const ibis::qContinuousRange*>(term)), ht);
	break;
    case ibis::qExpr::DRANGE:
	ierr = table0->doScan
	    (*(reinterpret_cast<const ibis::qDiscreteRange*>(term)), ht);
	break;
    case ibis::qExpr::ANYANY:
	table0->matchAny
	    (*(reinterpret_cast<const ibis::qAnyAny*>(term)), ht);
	break;
    case ibis::qExpr::STRING:
	table0->lookforString
	    (*(reinterpret_cast<const ibis::qString*>(term)), ht);
	if (ibis::gVerbose > 1)
	    logMessage("doScan", "NOTE -- scanning the index for "
		       "string comparisons");
	break;
    case ibis::qExpr::COMPRANGE:
	table0->doScan(*(reinterpret_cast<const ibis::compRange*>(term)),
		       ht);
	break;
    case ibis::qExpr::TOPK:
    case ibis::qExpr::JOIN: { // pretend every row qualifies
	ht.set(1, table0->nRows());
	ierr = -2;
	break;
    }
    default:
	logWarning("doScan", "unable to evaluate query term of "
		   "unknown type");
	ierr = -1;
    }
    if (ierr < 0) // no confirmed hits
	ht.set(0, table0->nRows());
    return ierr;
} // ibis::query::doScan

// masked sequential scan
int ibis::query::doScan(const ibis::qExpr* term, const ibis::bitvector& mask,
			ibis::bitvector& ht) const {
    int ierr = 0;
    if (term == 0) return ierr;
    if (mask.cnt() == 0) { // no hits
	ht.set(0, mask.size());
	return ierr;
    }
    LOGGER(ibis::gVerbose >= 8) << "query::[" << myID
	      << "]::doScan -- reading data entries to resolve " << *term
	      << " with mask.size() = " << mask.size() << " and mask.cnt() = "
	      << mask.cnt();

    switch (term->getType()) {
    case ibis::qExpr::LOGICAL_NOT: {
	ierr = doScan(term->getLeft(), mask, ht);
	if (ierr >= 0) {
	    ibis::bitvector* tmp = mask - ht;
	    ht.copy(*tmp);
	    delete tmp;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_AND: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), mask, b1);
	if (ierr >= 0)
	    ierr = doScan(term->getRight(), b1, ht);
	break;
    }
    case ibis::qExpr::LOGICAL_OR: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), mask, ht);
	// decide whether to update the mask use for the next evalutation
	// the reason for using the new mask is that we can avoid examining
	// the rows that already known to satisfy the query condition (i.e.,
	// already known to be hits)
	// want to make sure the cost of generating the new mask is less
	// than the time saved by using the new task
	// cost of generating new mask is roughly proportional
	// (mask.bytes() + ht.bytes())
	// the reduction in query evalution time is likely to be proportional
	// to ht.cnt()
	// since there are no good estimates on the coefficients, we will
	// simply directly compare the two
	if (ierr >= 0) {
	    if (ht.cnt() > mask.bytes() + ht.bytes()) {
		ibis::bitvector* newmask = mask - ht;
		ierr = doScan(term->getRight(), *newmask, b1);
		delete newmask;
	    }
	    else {
		ierr = doScan(term->getRight(), mask, b1);
	    }
	    if (ierr >= 0)
		ht |= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_XOR: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), mask, ht);
	if (ierr >= 0) {
	    ierr = doScan(term->getRight(), mask, b1);
	    if (ierr >= 0)
		ht ^= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_MINUS: {
	ibis::bitvector b1;
	ierr = doScan(term->getLeft(), mask, ht);
	if (ierr >= 0) {
	    ierr = doScan(term->getRight(), ht, b1);
	    if (ierr >= 0)
		ht -= b1;
	}
	break;
    }
    case ibis::qExpr::RANGE: {
#if defined(TEST_SCAN_OPTIONS)
	// there are five ways to perform the scan

	// (1) use the input mask directly.  In this case, ht would be the
	// answer, no more operations are required.  This will access
	// mask.cnt() rows and perform (mask.cnt() - (1-frac)*cnt1) setBit
	// operations to generate ht.

	// (2) use the mask for the index only (the first assignment of
	// iffy) and perform the positive comparisons.  This will access
	// cnt0 rows, perform cnt0*frac setBit operations and perform two
	// bitwise logical operations to generate ht (ht = mask - (iffy -
	// res)).

	// (3) use the mask for the index only and perform the negative
	// comparisons.  This will access cnt0 rows, perform cnt0*(1-frac)
	// setBit operations and perform one bitwise logical operation to
	// generate ht (ht = mask - res).

	// (4) use the combined mask (the second assignment of variable
	// iffy) and perform the positive comparisons.  This will access
	// cnt1 rows, perform cnt1*frac setBit operations and perform two
	// bitwise logical operations to generate ht (ht = mask - (iffy -
	// res)).

	// (5) use the combined mask and perform the negative comparisons.
	// This will access cnt1 rows, perform cnt1*(1-frac) setBit
	// operations, and one bitwise logical operation to generate ht (ht
	// = mask - res).

	// For the initial implementation (Feb 14, 2004), only options (4)
	// and (5) are considered.  To differentiate them, we need to
	// evaluate the difference in the bitwise logical operations in
	// terms of setBit operations.  To do this, we assume the cost of
	// each bitwise minus operation is proportional to the total size of
	// the two operands and each setBit operation is equivalent to
	// operating on two words in the bitwise logical operation.  To
	// compare options (4) and (5), we now need to estimate the size of
	// bitvector res.  To estimate the size of res, we assume it has the
	// same clustering factor as iffy and use frac to compute its bit
	// density.   The clustering factor of iffy can be computed by
	// assuming the bits in the bitvector are generated from a simple
	// Markov process.
	// Feb 17, 2004
	// Due to some unexpected difficulties in estimating the clustering
	// factor, we will implement only option (4) for now.
	// Feb 18, 2004
	// The cost difference between options (4) and (5) is
	// option (4) -- 2*sizeof(word_t)*iffy.cnt()*frac + iffy.bytes() +
	// markovSize(iffy.size(), frac*iffy.cnt(),
	// clusteringFactor(iffy.size(), iffy.cnt(), iffy.bytes()))
	// option (5) -- 2*sizeof(word_t)*iffy.cnt()*(1-frac)
	// Feb 19, 2004
	// To properly determine the parameters to choosing the difference
	// options, implement all five choices and use an global variable
	// ibis::_scan_option to determine the choice to use.  Once a
	// reasonable set of parameters are determined, we will remove the
	// global variable.
	ibis::horometer timer;
	timer.start();
	switch (ibis::_scan_option) {
	default:
	case 1: { // option 1 -- the original implementation use this one
	    ierr = table0->doScan
		(*(reinterpret_cast<const ibis::qRange*>(term)), mask, ht);
	    break;
	}
	case 2: { // option 2
	    ibis::bitvector iffy;
	    float frac = table0->getUndecidable
		(*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
		 iffy);
	    const uint32_t cnt0 = iffy.cnt();
	    if (cnt0 > 0) {
		ierr = table0->doScan
		    (*(reinterpret_cast<const ibis::qRange*>(term)),
		     iffy, ht);
		if (ierr >= 0) {
		    iffy -= ht;
		    ht.copy(mask);
		    ht -= iffy;
		}
	    }
	    else { // no row eliminated
		ht.copy(mask);
	    }
	    break;
	}
	case 3: { // option 3
	    ibis::bitvector iffy;
	    float frac = table0->getUndecidable
		(*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
		 iffy);
	    const uint32_t cnt0 = iffy.cnt();
	    if (cnt0 > 0) {
		ibis::bitvector comp;
		ierr = table0->negativeScan
		    (*(reinterpret_cast<const ibis::qRange*>(term)),
		     comp, iffy);
		if (ierr >= 0) {
		    ht.copy(mask);
		    ht -= comp;
		}
	    }
	    else { // no row eliminated
		ht.copy(mask);
	    }
	    break;
	}
	case 4: { // option 4
	    ibis::bitvector iffy;
	    float frac = table0->getUndecidable
		(*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
		 iffy);
	    const uint32_t cnt0 = iffy.cnt();
	    if (cnt0 > 0) {
		iffy &= mask;
		const uint32_t cnt1 = iffy.cnt();
		if (cnt1 > 0) {
		    ierr = table0->doScan
			(*(reinterpret_cast<const ibis::qRange*>(term)),
			 iffy, ht);
		    if (ierr >= 0) {
			iffy -= ht;
			ht.copy(mask);
			ht -= iffy;
		    }
		}
		else { // no row eliminated
		    ht.copy(mask);
		}
	    }
	    else { // no row eliminated
		ht.copy(mask);
	    }
	    break;
	}
	case 5: { // option 5
	    ibis::bitvector iffy;
	    float frac = table0->getUndecidable
		(*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
		 iffy);
	    const uint32_t cnt0 = iffy.cnt();
	    if (cnt0 > 0) {
		iffy &= mask;
		const uint32_t cnt1 = iffy.cnt();
		const double fudging=2*sizeof(ibis::bitvector::word_t);
// 		const double cf = ibis::bitvector::clusteringFactor
// 		    (iffy.size(), iffy.cnt(), iffy.bytes());
		if (cnt1 > 0) {
		    ibis::bitvector comp;
		    ierr = table0->negativeScan
			(*(reinterpret_cast<const ibis::qRange*>(term)),
			 comp, iffy);
		    if (ierr >= 0) {
			ht.copy(mask);
			ht -= comp;
		    }
		}
		else { // no row eliminated
		    ht.copy(mask);
		}
	    }
	    else { // no row eliminated
		ht.copy(mask);
	    }
	    break;
	}
	} // end of switch (ibis::_scan_option)
	timer.stop();
	logMessage("doScan", "Evaluating range condition (option %d) took "
		   "%g sec elapsed time", ibis::_scan_option,
		   timer.realTime());
#else
// 	// 05/11/05: commented out because it only works for ranges joined
// 	// together with AND operators! TODO: need something better
// 	// a combined version of option 4 and 5
// 	ibis::bitvector iffy;
// 	float frac = table0->getUndecidable
// 	    (*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
// 	     iffy);
// 	const uint32_t cnt0 = iffy.cnt();
// 	if (cnt0 > 0) {
// 	    iffy &= mask;
// 	    const uint32_t cnt1 = iffy.cnt();
// 	    if (cnt1 == 0) { // no row eliminated
// 		ht.copy(mask);
// 	    }
// 	    else if (cnt1 >= cnt0) { // directly use the input mask
// 		table0->doScan
// 		    (*(reinterpret_cast<const ibis::qRange*>(term)),
// 		     mask, ht);
// 	    }
// 	    else if (static_cast<int>(frac+frac) > 0) {
// 		// negative evaluation -- option 5
// 		ibis::bitvector comp;
// 		table0->negativeScan
// 		    (*(reinterpret_cast<const ibis::qRange*>(term)),
// 		     comp, iffy);
// 		ht.copy(mask);
// 		ht -= comp;
// 	    }
// 	    else {
// 		// direct evaluation -- option 4
// 		table0->doScan
// 		    (*(reinterpret_cast<const ibis::qRange*>(term)),
// 		     iffy, ht);
// 		iffy -= ht;
// 		ht.copy(mask);
// 		ht -= iffy;
// 	    }
// 	}
// 	else { // no row eliminated
// 	    ht.copy(mask);
// 	}
	ierr = table0->doScan
	    (*(reinterpret_cast<const ibis::qRange*>(term)), mask, ht);
#endif
	break;
    }
    case ibis::qExpr::DRANGE: {
	ierr = table0->doScan
	    (*(reinterpret_cast<const ibis::qDiscreteRange*>(term)), mask, ht);
	break;
    }
    case ibis::qExpr::ANYANY: {
	ierr = table0->matchAny
	    (*(reinterpret_cast<const ibis::qAnyAny*>(term)), mask, ht);
	break;
    }
    case ibis::qExpr::STRING: {
	ierr = table0->lookforString
	    (*(reinterpret_cast<const ibis::qString*>(term)), ht);
	ht &= mask;
	if (ibis::gVerbose > 1)
	    logMessage("doScan", "NOTE -- scanning the index for "
		       "string comparisons");
	break;
    }
    case ibis::qExpr::COMPRANGE: {
	ierr = table0->doScan
	    (*(reinterpret_cast<const ibis::compRange*>(term)), mask, ht);
	break;
    }
    case ibis::qExpr::TOPK:
    case ibis::qExpr::JOIN: { // pretend every row qualifies
	ht.copy(mask);
	ierr = -2;
	break;
    }
    default: {
	logWarning("doScan", "unable to evaluate query term of "
		   "unknown type");
	ht.set(0, table0->nRows());
	ierr = -1;
	break;}
    }
    if (ierr < 0) // no confirmed hits
	ht.set(0, table0->nRows());
#ifdef DEBUG
    ibis::util::logger lg(ibis::gVerbose-1);
    lg.buffer() << "ibis::query[" << myID << "]::doScan(" << *term
		<< ") --> " << ht.cnt() << ", ierr = " << ierr << "\n";
#if DEBUG + 0 > 1
    lg.buffer() << "ht \n" << ht;
#else
    if (ibis::gVerbose >= 30 || (ht.bytes() < (2 << ibis::gVerbose)))
	lg.buffer() << "ht \n" << ht;
#endif
#else
    LOGGER(ibis::gVerbose >= 5) << "ibis::query[" << myID << "]::doScan(" << *term
	      << ") --> " << ht.cnt() << ", ierr = " << ierr;
#endif
    return ierr;
} // ibis::query::doScan

// combines the operations on index and the sequential scan in one function
int ibis::query::doEvaluate(const ibis::qExpr* term,
			    ibis::bitvector& ht) const {
    if (term == 0) { // no hits
	ht.set(0, table0->nRows());
	return 0;
    }
    LOGGER(ibis::gVerbose >= 6) << "query[" << myID
	      << "]::doEvaluate -- starting to evaluate " << *term;

    int ierr = 0;
    switch (term->getType()) {
    case ibis::qExpr::LOGICAL_NOT: {
	ierr = doEvaluate(term->getLeft(), ht);
	if (ierr >= 0)
	    ht.flip();
	break;
    }
    case ibis::qExpr::LOGICAL_AND: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), b1);
	if (ierr >= 0)
	    ierr = doEvaluate(term->getRight(), b1, ht);
	break;
    }
    case ibis::qExpr::LOGICAL_OR: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), ht);
	if (ierr >= 0) {
	    ierr = doEvaluate(term->getRight(), b1);
	    if (ierr >= 0)
		ht |= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_XOR: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), ht);
	if (ierr >= 0) {
	    ierr = doEvaluate(term->getRight(), b1);
	    if (ierr >= 0)
		ht ^= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_MINUS: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), ht);
	if (ierr >= 0) {
	    ierr = doEvaluate(term->getRight(), ht, b1);
	    if (ierr >= 0)
		ht -= b1;
	}
	break;
    }
    case ibis::qExpr::RANGE: {
	ibis::bitvector tmp;
	tmp.set(1, table0->nRows());
	ierr = table0->evaluateRange
	    (*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
	     tmp, ht);
	if (ierr < 0) {
	    ierr = table0->estimateRange
		(*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
		 ht, tmp);
	    if (ierr >= 0 && ht.size() == tmp.size() && ht.cnt() < tmp.cnt()) {
		// estimateRange produced two bounds as the solution, need to
		// scan some entries to determine exactly which satisfy the
		// condition
		tmp -= ht; // tmp now contains entries to be scanned
		ibis::bitvector res;
		ierr = table0->doScan
		    (*(reinterpret_cast<const ibis::qRange*>(term)), tmp, res);
		if (ierr >= 0)
		    ht |= res;
	    }
	}
	break;
    }
    case ibis::qExpr::DRANGE: { // call evalauteRange, use doScan on failure
	ierr = table0->evaluateRange
	    (*(reinterpret_cast<const ibis::qDiscreteRange*>(term)),
	     table0->getMask(), ht);
	if (ierr < 0) { // revert to estimate and scan
	    ibis::bitvector tmp;
	    ierr = table0->estimateRange
		(*(reinterpret_cast<const ibis::qDiscreteRange*>(term)),
		 ht, tmp);
	    if (ierr >= 0 && ht.size() == tmp.size() && ht.cnt() < tmp.cnt()) {
		// estimateRange produced two bounds as the solution, need to
		// scan some entries to determine exactly which satisfy the
		// condition
		tmp -= ht; // tmp now contains entries to be scanned
		ibis::bitvector res;
		ierr = table0->doScan
		    (*(reinterpret_cast<const ibis::qRange*>(term)), tmp, res);
		if (ierr >= 0)
		    ht |= res;
	    }
	}
	break;
    }
    case ibis::qExpr::STRING: {
	table0->lookforString
	    (*(reinterpret_cast<const ibis::qString*>(term)), ht);
	break;
    }
    case ibis::qExpr::COMPRANGE: {
	ierr = table0->doScan
	    (*(reinterpret_cast<const ibis::compRange*>(term)), ht);
	break;
    }
    case ibis::qExpr::ANYANY: {
	const ibis::qAnyAny *tmp =
	    reinterpret_cast<const ibis::qAnyAny*>(term);
	ibis::bitvector more;
	table0->estimateMatchAny(*tmp, ht, more);
	if (ht.size() == more.size() && ht.cnt() < more.cnt()) {
	    more -= ht;
	    if (more.cnt() > 0) {
		ibis::bitvector res;
		table0->matchAny(*tmp, res, more);
		ht |= res;
	    }
	}
	break;
    }
    case ibis::qExpr::TOPK:
    case ibis::qExpr::JOIN: { // pretend every row qualifies
	ht.set(1, table0->nRows());
	break;
    }
    default:
	logWarning("doEvaluate", "unable to evaluate query term of "
		   "unknown type, presume every row is a hit");
	ht.set(0, table0->nRows());
	ierr = -1;
    }
#ifdef DEBUG
    ibis::util::logger lg(4);
    lg.buffer() << "ibis::query[" << myID << "]::doEvaluate(" << *term
		<< ") --> " << ht.cnt() << ", ierr = " << ierr << "\n";
#if DEBUG + 0 > 1
    lg.buffer() << "ht \n" << ht;
#else
    if (ibis::gVerbose >= 30 || (ht.bytes() < (2 << ibis::gVerbose)))
	lg.buffer() << "ht \n" << ht;
#endif
#else
    LOGGER(ibis::gVerbose >= 5) << "ibis::query[" << myID << "]::doEvaluate(" << *term
	      << ") --> " << ht.cnt() << ", ierr = " << ierr;
#endif
    return ierr;
} // ibis::query::doEvaluate

// combines the operations on index and the sequential scan in one function
int ibis::query::doEvaluate(const ibis::qExpr* term,
			    const ibis::bitvector& mask,
			    ibis::bitvector& ht) const {
    int ierr = 0;
    if (term == 0) { // no hits
	ht.set(0, table0->nRows());
	return ierr;
    }
    if (mask.cnt() == 0) { // no hits
	ht.set(0, mask.size());
	return ierr;
    }
    LOGGER(ibis::gVerbose >= 8) << "query[" << myID << "]::doEvaluate -- starting to evaluate "
	      << *term;

    switch (term->getType()) {
    case ibis::qExpr::LOGICAL_NOT: {
	ierr = doEvaluate(term->getLeft(), mask, ht);
	if (ierr >= 0) {
	    ht.flip();
	    ht &= mask;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_AND: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), mask, b1);
	if (ierr >= 0)
	    ierr = doEvaluate(term->getRight(), b1, ht);
	break;
    }
    case ibis::qExpr::LOGICAL_OR: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), mask, ht);
	if (ierr >= 0) {
	    if (ht.cnt() > mask.bytes() + ht.bytes()) {
		ibis::bitvector* newmask = mask - ht;
		ierr = doEvaluate(term->getRight(), *newmask, b1);
		delete newmask;
	    }
	    else {
		ierr = doEvaluate(term->getRight(), mask, b1);
	    }
	    if (ierr >= 0)
		ht |= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_XOR: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), mask, ht);
	if (ierr >= 0) {
	    ierr = doEvaluate(term->getRight(), mask, b1);
	    if (ierr >= 0)
		ht ^= b1;
	}
	break;
    }
    case ibis::qExpr::LOGICAL_MINUS: {
	ibis::bitvector b1;
	ierr = doEvaluate(term->getLeft(), mask, ht);
	if (ierr >= 0) {
	    ierr = doEvaluate(term->getRight(), ht, b1);
	    if (ierr >= 0)
		ht -= b1;
	}
	break;
    }
    case ibis::qExpr::RANGE: {
	ierr = table0->evaluateRange
	    (*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
	     mask, ht);
	if (ierr < 0) {
	    ibis::bitvector tmp;
	    ierr = table0->estimateRange
		(*(reinterpret_cast<const ibis::qContinuousRange*>(term)),
		 ht, tmp);
	    if (ierr >= 0) {
		if (ht.size() != tmp.size() || ht.cnt() >= tmp.cnt()) {
		    // tmp is taken to be the same as ht, i.e., estimateRange
		    // produced an exactly solution
		    ht &= mask;
		}
		else { // estimateRange produced an approximate solution
		    tmp -= ht;
		    ht &= mask;
		    tmp &= mask;
		    ibis::bitvector res;
		    ierr = table0->doScan
			(*(reinterpret_cast<const ibis::qRange*>(term)),
			 tmp, res);
		    if (ierr >= 0)
			ht |= res;
		}
	    }
	}
	break;
    }
    case ibis::qExpr::DRANGE: { // try evaluateRange, then doScan
	ierr = table0->evaluateRange
	    (*(reinterpret_cast<const ibis::qDiscreteRange*>(term)), mask, ht);
	if (ierr < 0) { // revert to estimate and scan
	    ibis::bitvector tmp;
	    ierr = table0->estimateRange
		(*(reinterpret_cast<const ibis::qDiscreteRange*>(term)),
		 ht, tmp);
	    if (ierr >= 0) {
		if (ht.size() != tmp.size() || ht.cnt() >= tmp.cnt()) {
		    // tmp is taken to be the same as ht, i.e., estimateRange
		    // produced an exactly solution
		    ht &= mask;
		}
		else { // estimateRange produced an approximate solution
		    tmp -= ht;
		    ht &= mask;
		    tmp &= mask;
		    ibis::bitvector res;
		    ierr = table0->doScan
			(*(reinterpret_cast<const ibis::qRange*>(term)),
			 tmp, res);
		    if (ierr >= 0)
			ht |= res;
		}
	    }
	}
	break;
    }
    case ibis::qExpr::STRING: {
	ierr = table0->lookforString
	    (*(reinterpret_cast<const ibis::qString*>(term)), ht);
	ht &= mask;
	break;
    }
    case ibis::qExpr::COMPRANGE: {
	ierr = table0->doScan
	    (*(reinterpret_cast<const ibis::compRange*>(term)), mask, ht);
	break;
    }
    case ibis::qExpr::ANYANY: {
	const ibis::qAnyAny *tmp =
	    reinterpret_cast<const ibis::qAnyAny*>(term);
	ibis::bitvector more;
	ierr = table0->estimateMatchAny(*tmp, ht, more);
	ht &= mask;
	if (ht.size() == more.size() && ht.cnt() < more.cnt()) {
	    more -= ht;
	    more &= mask;
	    if (more.cnt() > 0) {
		ibis::bitvector res;
		table0->matchAny(*tmp, more, res);
		ht |= res;
	    }
	}
	break;
    }
    case ibis::qExpr::TOPK:
    case ibis::qExpr::JOIN: { // pretend every row qualifies
	ht.copy(mask);
	break;
    }
    default:
	logWarning("doEvaluate", "unable to evaluate a query term of "
		   "unknown type, copy the mask as the solution");
	ht.set(0, mask.size());
	ierr = -1;
    }
#ifdef DEBUG
    ibis::util::logger lg;
    lg.buffer() << "ibis::query[" << myID << "]::doEvaluate(" << *term
		<< ", mask.cnt()=" << mask.cnt() << ") --> " << ht.cnt()
		<< ", ierr = " << ierr << "\n";
#if DEBUG + 0 > 1
    lg.buffer() << "ht \n" << ht;
#else
    if (ibis::gVerbose >= 30 || (ht.bytes() < (2U << ibis::gVerbose)))
	lg.buffer() << "ht \n" << ht;
#endif
#else
    LOGGER(ibis::gVerbose >= 4) << "ibis::query[" << myID << "]::doEvaluate(" << *term
	      << ", mask.cnt()=" << mask.cnt() << ") --> " << ht.cnt()
	      << ", ierr = " << ierr;
#endif
    return ierr;
} // ibis::query::doEvaluate

// a function to read the query file in a directory -- used by the
// constructor that takes a directory name as the argument
// the file contains:
// user id
// dataset name
// list of components
// query state
// time stamp on the dataset
// query condition or <NULL>
// list of OIDs
void ibis::query::readQuery(const ibis::partList& tl) {
    if (myDir == 0)
	return;

    char* ptr;
    char fn[MAX_LINE];
    strcpy(fn, myDir);
    strcat(fn, "query");

    long i;
    FILE* fptr = fopen(fn, "r");
    if (fptr == 0) {
	logWarning("readQuery", "unable to open query file \"%s\" ... %s", fn,
		   (errno ? strerror(errno) : "no free stdio stream"));
	clear(); // clear the files and directory
	return;
    }

    // user id
    fgets(fn, MAX_LINE, fptr);
    delete [] user;
    ptr = fn + strlen(fn);
    -- ptr;
    while (isspace(*ptr)) {
	*ptr = 0;
	-- ptr;
    }
    user = ibis::util::strnewdup(fn);

    // table names
    fgets(fn, MAX_LINE, fptr);
    ptr = fn + strlen(fn);
    -- ptr;
    while (isspace(*ptr)) {
	*ptr = 0;
	-- ptr;
    }
    ibis::partList::const_iterator it = tl.find(fn);
    if (it != tl.end()) { // found the table
	table0 = (*it).second;
    }
    else { // table name is not valid
	state = UNINITIALIZED;
	delete [] user;
	user = 0;
	return;
    }

    // select clause
    fgets(fn, MAX_LINE, fptr);
    ptr = fn + strlen(fn);
    -- ptr;
    while (isspace(*ptr)) {
	*ptr = 0;
	-- ptr;
    }
    if (strnicmp(fn, "<NULL>", 6))
	setSelectClause(fn);

    // table state (read as an integer)
    fscanf(fptr, "%ld", &i);
    state = (QUERY_STATE) i;

    // time stamp (read as an integer)
    fscanf(fptr, "%ld", &dstime);

    // where clause or RID list
    fgets(fn, MAX_LINE, fptr); // skip the END_OF_LINE character
    fgets(fn, MAX_LINE, fptr);
    ptr = fn + strlen(fn);
    -- ptr;
    while (isspace(*ptr)) {
	*ptr = 0;
	-- ptr;
    }
    if (strcmp(fn, "<NULL>")) { // not NONE
	setWhereClause(fn);
    }
    else { // read the remaining part of the file to fill rids_in
	rids_in->clear();
	unsigned tmp[2];
	while (fscanf(fptr, "%u %u", tmp, tmp+1) == 2) {
	    ibis::rid_t rid;
	    rid.num.run = *tmp;
	    rid.num.event = tmp[1];
	    rids_in->push_back(rid);
	}
    }
    fclose(fptr);
} // ibis::query::readQuery

// write the content of the current query into a file
void ibis::query::writeQuery() {
    if (myDir == 0)
	return;

    char fn[PATH_MAX];
    strcpy(fn, myDir);
    strcat(fn, "query");

    FILE* fptr = fopen(fn, "w");
    if (fptr == 0) {
	logWarning("writeQuery", "failed to open file \"%s\" ... %s", fn,
		   (errno ? strerror(errno) : "no free stdio stream"));
	return;
    }

    if (comps.size() > 0)
	fprintf(fptr, "%s\n%s\n%s\n%d\n", user, table0->name(),
		*comps, (int)state);
    else
	fprintf(fptr, "%s\n%s\n<NULL>\n%d\n", user, table0->name(),
		(int)state);
    fprintf(fptr, "%ld\n", dstime);
    if (condition) {
	fprintf(fptr, "%s\n", condition);
    }
    else if (expr != 0) {
	std::ostringstream ostr;
	ostr << *expr;
	fprintf(fptr, "%s\n", ostr.str().c_str());
    }
    else {
	fprintf(fptr, "<NULL>\n");
    }
    if (rids_in) {
	ibis::RIDSet::const_iterator it;
	for (it = rids_in->begin(); it != rids_in->end(); ++it) {
	    fprintf(fptr, "%lu %lu\n",
		    static_cast<long unsigned>((*it).num.run),
		    static_cast<long unsigned>((*it).num.event));
	}
    }
    fclose(fptr);
} // ibis::query::writeQuery

void ibis::query::readHits() {
    if (myDir == 0)
	return;

    char fn[PATH_MAX];
    strcpy(fn, myDir);
    strcat(fn, "hits");
    if (hits == 0)
	hits = new ibis::bitvector;
    hits->read(fn);
    sup = hits;
} // ibis::query::readHits

void ibis::query::writeHits() const {
    if (hits != 0 && myDir != 0) {
	char fn[PATH_MAX];
	strcpy(fn, myDir);
	strcat(fn, "hits");
	hits->write(fn); // write hit vector
    }
} // ibis::query::writeHits

// read RIDs from the file named "rids" and return a pointer to
// ibis::RIDSet
ibis::RIDSet* ibis::query::readRIDs() const {
    if (myDir == 0)
	return 0;

    char fn[PATH_MAX];
    strcpy(fn, myDir);
    strcat(fn, "rids");

    ibis::RIDSet* rids = new ibis::RIDSet();
    int ierr = ibis::fileManager::instance().getFile(fn, *rids);
    if (ierr != 0) {
	logWarning("readRIDs", "unable to open file \"%s\"", fn);
	remove(fn); // attempt to remove it
	delete rids;
	rids = 0;
    }
    else {
#if defined(DEBUG)
	ibis::util::logger lg;
	lg.buffer() << "query[" << myID << "::readRIDs() got " << rids->size()
		    << "\n";
	for (ibis::RIDSet::const_iterator it = rids->begin();
	     it != rids->end(); ++it)
	    lg.buffer() << (*it) << "\n";
#endif
	if (rids->size() == 0) {
	    delete rids;
	    rids = 0;
	}
    }
    return rids;
} // ibis::query::readRIDs

void ibis::query::writeRIDs(const ibis::RIDSet* rids) const {
    if (rids && myDir) {
	char *fn = new char[strlen(myDir) + 8];
	strcpy(fn, myDir);
	strcat(fn, "rids");
	rids->write(fn);
	delete [] fn;
    }
} // ibis::query::writeRIDs

// the function to clear most of the resouce consuming parts of a query
void ibis::query::clear() {
    if (ibis::gVerbose > 4)
	logMessage("clear",
		   "clearing all stored information about the query");

    writeLock lck(this, "clear");
    comps.clear();
    // clear all pointers to in-memory resrouces
    delete [] condition;
    condition = 0;
    delete expr;
    expr = 0;
    delete rids_in;
    rids_in = 0;

    if (hits == sup) { // remove bitvectors
	delete hits;
	hits = 0;
	sup = 0;
    }
    else {
	delete hits;
	delete sup;
	hits = 0;
	sup = 0;
    }
    if (dslock) { // remove read lock on the associated table
	delete dslock;
	dslock = 0;
    }

    if (myDir) {
	ibis::fileManager::instance().flushDir(myDir);
	std::string pnm = "query.";
	pnm += myID;
	pnm += ".purgeTempFiles";
	if (ibis::gParameters().isTrue(pnm.c_str())) {
	    ibis::util::removeDir(myDir);
	    if (ibis::gVerbose > 6)
		logMessage("clear", "removed %s", myDir);
	}
    }
} // ibis::query::clear

void ibis::query::removeFiles() {
    if (dslock != 0) { // release read lock on table
	delete dslock;
	dslock = 0;
    }

    if (myDir == 0) return;
    // remove all files generated for this query and recreate the directory
    uint32_t len = strlen(myDir);
    char* fname = new char[len + 16];
    strcpy(fname, myDir);
    strcat(fname, "query");
    if (0 == remove(fname)) {
	if (ibis::gVerbose > 6)
	    logMessage("clear", "removed %s", fname);
    }
    else if (errno != ENOENT || ibis::gVerbose > 7)
	logMessage("clear", "unable to remove %s ... %s", fname,
		   strerror(errno));

    strcpy(fname+len, "hits");
    ibis::fileManager::instance().flushFile(fname);
    if (0 == remove(fname)) {
	if (ibis::gVerbose > 6)
	    logMessage("clear", "removed %s", fname);
    }
    else if (errno != ENOENT || ibis::gVerbose > 7)
	logMessage("clear", "unable to remove %s ... %s", fname,
		   strerror(errno));

    strcpy(fname+len, "rids");
    ibis::fileManager::instance().flushFile(fname);
    if (0 == remove(fname)) {
	if (ibis::gVerbose > 6)
	    logMessage("clear", "removed %s", fname);
    }
    else if (errno != ENOENT || ibis::gVerbose > 7)
	logMessage("clear", "unable to remove %s ... %s", fname,
		   strerror(errno));

    strcpy(fname+len, "fids");
    ibis::fileManager::instance().flushFile(fname);
    if (0 == remove(fname)) {
	if (ibis::gVerbose > 6)
	    logMessage("clear", "removed %s", fname);
    }
    else if (errno != ENOENT || ibis::gVerbose > 7)
	logMessage("clear", "unable to remove %s ... %s", fname,
		   strerror(errno));

    strcpy(fname+len, "bundles");
    ibis::fileManager::instance().flushFile(fname);
    if (0 == remove(fname)) {
	if (ibis::gVerbose > 6)
	    logMessage("clear", "removed %s", fname);
    }
    else if (errno != ENOENT || ibis::gVerbose > 7)
	logMessage("clear", "unable to remove %s ... %s", fname,
		   strerror(errno));
    delete [] fname;
} // ibis::query::removeFiles

void ibis::query::printSelected(std::ostream& out) const {
    if (comps.empty()) return;
    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED) {
	ibis::bundle* bdl = 0;
	if (hits != 0)
	    if (hits->cnt() > 0)
		bdl = ibis::bundle::create(*this);
	if (bdl != 0) {
	    bdl->print(out);
	    bdl->write(*this);
	    delete bdl;
	}
	else {
	    logWarning("printSelected", "unable to construct ibis::bundle");
	}
    }
    else {
	logWarning("printSelected", "must perform full estimate before "
		   "calling this function");
    }
} // ibis::query::printSelected

void ibis::query::printSelectedWithRID(std::ostream& out) const {
    if (state == FULL_EVALUATE || state == BUNDLES_TRUNCATED ||
	state == HITS_TRUNCATED) {
	ibis::bundle* bdl = 0;
	if (hits != 0)
	    if (hits->cnt() > 0)
		bdl = ibis::bundle::create(*this);
	if (bdl != 0) {
	    bdl->printAll(out);
	    bdl->write(*this);
	    delete bdl;
	}
	else {
	    logWarning("printSelectedWithRID",
		       "unable to construct ibis::bundle");
	}
    }
    else {
	logWarning("printSelectedWithRID", "must perform full estimate "
		   "before calling this function");
    }
} // ibis::query::printSelectedWithRID

uint32_t ibis::query::countPages(unsigned wordsize) const {
    uint32_t res = 0;
    if (hits == 0)
	return res;
    if (hits->cnt() == 0)
	return res;
    if (wordsize == 0)
	return res;

    // words per page
    const uint32_t wpp = ibis::fileManager::pageSize() / wordsize;
    uint32_t last;  // the position of the last entry encountered
    ibis::bitvector::indexSet ix = hits->firstIndexSet();
    last = *(ix.indices());
    if (ibis::gVerbose < 8) {
	while (ix.nIndices() > 0) {
	    const ibis::bitvector::word_t *ind = ix.indices();
	    const uint32_t p0 = *ind / wpp;
	    res += (last < p0*wpp); // last not on the current page
	    if (ix.isRange()) {
		res += (ind[1] / wpp - p0);
		last = ind[1];
	    }
	    else {
		last = ind[ix.nIndices()-1];
		res += (last / wpp > p0);
	    }
	    ++ ix;
	}
    }
    else {
	ibis::util::logger lg;
	lg.buffer() << "ibis::query[" << myID << "]::countPages(" << wordsize
		    << ") page numbers: ";
	for (size_t i = 0; ix.nIndices() > 0 && (i >> ibis::gVerbose) == 0;
	     ++ i) {
	    const ibis::bitvector::word_t *ind = ix.indices();
	    const uint32_t p0 = *ind / wpp;
	    if (last < p0*wpp) { // last not on the current page
		lg.buffer() << last/wpp << " ";
		++ res;
	    }
	    if (ix.isRange()) {
		const unsigned mp = (ind[1]/wpp - p0);
		if (mp > 1) {
		    lg.buffer() << p0 << "*" << mp << " ";
		}
		else if (mp > 0) {
		    lg.buffer() << p0 << " ";
		}
		res += mp;
		last = ind[1];
	    }
	    else {
		last = ind[ix.nIndices()-1];
		if (last / wpp > p0) {
		    lg.buffer() << p0 << " ";
		    ++ res;
		}
	    }
	    ++ ix;
	}
	if (ix.nIndices() > 0)
	    lg.buffer() << " ...";
    }
    return res;
} // ibis::query::countPages

int ibis::query::doExpand(ibis::qExpr* exp0) const {
    int ret = 0;
    switch (exp0->getType()) {
    case ibis::qExpr::LOGICAL_AND:
    case ibis::qExpr::LOGICAL_OR:
    case ibis::qExpr::LOGICAL_XOR: { // binary operators
	ret = doExpand(exp0->getLeft());
	ret += doExpand(exp0->getRight());
	break;
    }
    case ibis::qExpr::LOGICAL_NOT: { // negation operator
	ret = doContract(exp0->getLeft());
	break;
    }
    case ibis::qExpr::RANGE: { // a range condition
	ibis::qContinuousRange* range =
	    reinterpret_cast<ibis::qContinuousRange*>(exp0);
	ibis::column* col = table0->getColumn(range->colName());
	ret = col->expandRange(*range);
	break;
    }
    default:
	break;
    }
    return ret;
} // ibis::query::doExpand

int ibis::query::doContract(ibis::qExpr* exp0) const {
    int ret = 0;
    switch (exp0->getType()) {
    case ibis::qExpr::LOGICAL_AND:
    case ibis::qExpr::LOGICAL_OR:
    case ibis::qExpr::LOGICAL_XOR: { // binary operators
	ret = doContract(exp0->getLeft());
	ret += doContract(exp0->getRight());
	break;
    }
    case ibis::qExpr::LOGICAL_NOT: { // negation operator
	ret = doExpand(exp0->getLeft());
	break;
    }
    case ibis::qExpr::RANGE: { // a range condition
	ibis::qContinuousRange* range =
	    reinterpret_cast<ibis::qContinuousRange*>(exp0);
	ibis::column* col = table0->getColumn(range->colName());
	ret = col->contractRange(*range);
	break;
    }
    default:
	break;
    }
    return ret;
} // ibis::query::doContract

void ibis::query::addJoinConstraints(ibis::qExpr*& exp0) const {
    std::vector<const ibis::rangeJoin*> terms;
    ibis::qExpr::simplify(exp0);
    exp0->extractJoins(terms);
    if (terms.empty()) // no join terms to use
	return;

    LOGGER(ibis::gVerbose >= 7) << "ibis::query[" << myID
	      << "]::addJoinConstraints -- current query expression\n"
	      << *exp0;

    for (uint32_t i = 0; i < terms.size(); ++ i) {
	const ibis::rangeJoin* jn = terms[i];
	double delta = 0.0;
	if (jn->getRange()) {
	    const ibis::compRange::term *tm = jn->getRange();
	    if (tm != 0) {
		if (tm->termType() != ibis::compRange::NUMBER)
		    continue;
		else
		    delta = tm->eval();
	    }
	}

	const char *nm1 = jn->getName1();
	const char *nm2 = jn->getName2();
	const ibis::column *col1 = table0->getColumn(nm1);
	const ibis::column *col2 = table0->getColumn(nm2);
	if (col1 == 0 || col2 == 0)
	    continue;

	double cmin1 = col1->getActualMin();
	double cmax1 = col1->getActualMax();
	double cmin2 = col2->getActualMin();
	double cmax2 = col2->getActualMax();
	ibis::qRange* cur1 = exp0->findRange(nm1);
	ibis::qRange* cur2 = exp0->findRange(nm2);
	if (cur1) {
	    double tmp = cur1->leftBound();
	    if (tmp > cmin1)
		cmin1 = tmp;
	    tmp = cur1->rightBound();
	    if (tmp < cmax1)
		cmax1 = tmp;
	}
	if (cur2) {
	    double tmp = cur2->leftBound();
	    if (tmp > cmin2)
		cmin2 = tmp;
	    tmp = cur2->rightBound();
	    if (tmp < cmax2)
		cmax2 = tmp;
	}

	if (cmin1 < cmin2-delta || cmax1 > cmax2+delta) {
	    double bd1 = (cmin1 >= cmin2-delta ? cmin1 : cmin2-delta);
	    double bd2 = (cmax1 <= cmax2+delta ? cmax1 : cmax2+delta);
	    if (cur1) { // reduce the range of an existing range condition
		cur1->restrictRange(bd1, bd2);
	    }
	    else { // add an addition term of nm1
		ibis::qContinuousRange *qcr =
		    new ibis::qContinuousRange(bd1, ibis::qExpr::OP_LE,
					       nm1, ibis::qExpr::OP_LE,
					       bd2);
		ibis::qExpr *qop = new ibis::qExpr(ibis::qExpr::LOGICAL_AND,
						   qcr, exp0->getRight());
		exp0->getRight() = qop;
	    }
	}

	if (cmin2 < cmin1-delta || cmax2 > cmax1+delta) {
	    double bd1 = (cmin2 >= cmin1-delta ? cmin2 : cmin1-delta);
	    double bd2 = (cmax2 <= cmax1+delta ? cmax2 : cmax1+delta);
	    if (cur2) {
		cur2->restrictRange(bd1, bd2);
	    }
	    else {
		ibis::qContinuousRange *qcr =
		    new ibis::qContinuousRange(bd1, ibis::qExpr::OP_LE,
					       nm2, ibis::qExpr::OP_LE,
					       bd2);
		ibis::qExpr *qop = new ibis::qExpr(ibis::qExpr::LOGICAL_AND,
						   qcr, exp0->getLeft());
		exp0->getLeft() = qop;
	    }
	}
    }
    LOGGER(ibis::gVerbose >= 7) << "ibis::query[" << myID << "]::addJoinConstraints -- "
	"query expression with additional constraints\n"
	      << *exp0;
} // ibis::query::addJoinConstraints

/// This function only counts the number of hits; it does produce the
/// actual tuples for the results of join.  Additionally, it performs only
/// self-join, i.e., join a table with itself.  This is only meant to test
/// some algorithms for evaluating joins.
int64_t ibis::query::processJoin() {
    int64_t ret = 0;
    if (expr == 0) return ret;
    //     if (state != ibis::query::FULL_EVALUATE ||
    // 	timestamp() != partition()->timestamp())
    // 	evaluate();
    if (hits == 0 || hits->cnt() == 0) return ret; // no hits

    ibis::horometer timer;
    std::vector<const ibis::rangeJoin*> terms;

    // extract all rangeJoin objects from the root of the expression tree
    // to the first operator that is not AND
    expr->extractJoins(terms);
    if (terms.empty())
	return ret;

    // put those can be evaluated with indices at the end of the list
    uint32_t ii = 0;
    uint32_t jj = terms.size() - 1;
    while (ii < jj) {
	if (terms[jj]->getRange() == 0)
	    -- jj;
	else if (terms[jj]->getRange()->termType() ==
		 ibis::compRange::NUMBER)
	    -- jj;
	else {
	    ibis::compRange::barrel baj(terms[jj]->getRange());
	    if (baj.size() == 0 ||
		(baj.size() == 1 &&
		 0 == stricmp(baj.name(0), terms[jj]->getName1()))) {
		-- jj;
	    }
	    else if (terms[ii]->getRange() != 0 &&
		     terms[ii]->getRange()->termType() !=
		     ibis::compRange::NUMBER) {
		ibis::compRange::barrel bai(terms[ii]->getRange());
		if (bai.size() > 1 ||
		    (bai.size() == 1 &&
		     0 != stricmp(bai.name(0), terms[ii]->getName1()))) {
		    ++ ii;
		}
		else { // swap
		    const ibis::rangeJoin *tmp = terms[ii];
		    terms[ii] = terms[jj];
		    terms[jj] = tmp;
		    ++ ii;
		    -- jj;
		}
	    }
	    else { // swap
		const ibis::rangeJoin *tmp = terms[ii];
		terms[ii] = terms[jj];
		terms[jj] = tmp;
		++ ii;
		-- jj;
	    }
	}
    }

    const ibis::bitvector64::word_t npairs =
	static_cast<ibis::bitvector64::word_t>(table0->nRows()) *
	table0->nRows();
    // retrieve two column pointers for future operations
    const ibis::column *col1 = table0->getColumn(terms.back()->getName1());
    const ibis::column *col2 = table0->getColumn(terms.back()->getName2());
    while ((col1 == 0 || col2 == 0) && terms.size() > 0) {
	std::ostringstream ostr;
	ostr << *(terms.back());
	logWarning("processJoin", "either %s or %s from table %s is not a "
		   "valid column name in table %s", terms.back()->getName1(),
		   terms.back()->getName2(), ostr.str().c_str(),
		   table0->name());
	terms.resize(terms.size()-1); // remove the invalid term
	if (terms.size() > 0) {
	    col1 = table0->getColumn(terms.back()->getName1());
	    col2 = table0->getColumn(terms.back()->getName2());
	}
    }
    if (terms.empty()) {
	logWarning("processJoin", "nothing left in the std::vector terms");
	ret = -1;
	return ret;
    }
    std::ostringstream outstr;
    outstr << "processed (" << *(terms.back());
    for (ii = 1; ii < terms.size(); ++ ii)
	outstr << " AND " << *(terms[ii]);

    ibis::bitvector mask;
    // generate a mask
    col1->getNullMask(mask);
    if (col2 != col1) {
	ibis::bitvector tmp;
	col2->getNullMask(tmp);
	mask &= tmp;
    }
    if (sup != 0 && sup->cnt() > hits->cnt()) {
	mask &= *sup;
    }
    else {
	mask &= *hits;
    }

    int64_t cnt = 0;
    { // OPTION 0 -- directly read the values
	ibis::horometer watch;
	watch.start();
	cnt = table0->evaluateJoin(terms, mask);
	watch.stop();
	//logMessage("processJoin", "OPTION 0 -- loop join computes %lld "
	//	   "hits in %g seconds", cnt, watch.realTime());
	if (cnt >= 0) {
	    LOGGER(ibis::gVerbose >= 0)
		<< "processJoin with OPTION 0 -- loop join computed "
		<< cnt << " hits, took " << watch.realTime() << " sec";
	}
    }
    { // OPTION 1 -- sort-merge join
	ibis::horometer watch;
	watch.start();
	cnt = sortJoin(terms, mask);
	watch.stop();
	LOGGER(ibis::gVerbose >= 0)
	    << "processJoin with OPTION 1 -- sort-merge join computed "
	    << cnt << " hits, took " << watch.realTime() << " sec";
    }

    ibis::column::indexLock idy1(col1, "processJoin");
    ibis::column::indexLock idy2(col2, "processJoin");
    const ibis::index *idx1 = idy1.getIndex();
    const ibis::index *idx2 = idy2.getIndex();
    const ibis::qRange *range1 = expr->findRange(col1->name());
    const ibis::qRange *range2 = expr->findRange(col2->name());
    if (idx1 != 0 && idx2 != 0 && terms.size() == 1 &&
	idx1->type() == ibis::index::RELIC &&
	idx2->type() == ibis::index::RELIC) {
	// OPTION 2 -- using relic indices to count the number of hits only
	ibis::horometer tm1, tm2;
	tm1.start();
	int64_t cnt2 = reinterpret_cast<const ibis::relic*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::relic*>(idx2),
		     *(terms.back()), mask);
	tm1.stop();
	// OPTION 3 -- use relic indices to count the number of hits only
	tm2.start();
	int64_t cnt3 = reinterpret_cast<const ibis::relic*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::relic*>(idx2),
		     *(terms.back()), mask, range1, range2);
	tm2.stop();

	ibis::util::logger lg;
	lg.buffer() << "processJoin with OPTION 2 -- basic bitmap index + "
	    "bitmap mask ";
	if (terms.size() == 1)
	    lg.buffer() << "computed ";
	else
	    lg.buffer() << "estimated (baed on " << *(terms.back())
			<< ") to be no more than ";
	lg.buffer() << cnt2 << " hits, took "
		    << tm1.realTime() << " sec\n"
		    << "processJoin with OPTION 3 -- basic bitmap index + "
	    "bitmap mask and ";
	if (range1) {
	    if (range2) {
		lg.buffer() << "two range constraints (" << *range1
			    << " and " << *range2 << ")";
	    }
	    else {
		lg.buffer() << "one range constraint (" << *range1 << ")";
	    }
	}
	else if (range2) {
	    lg.buffer() << "one range constraint (" << *range2 << ")";
	}
	else {
	    lg.buffer() << "no range constraint";
	}
	if (terms.size() == 1)
	    lg.buffer() << " computed ";
	else
	    lg.buffer() << "estimated (baed on " << *(terms.back())
			<< ") to be no more than ";
	lg.buffer() << cnt3 << " hits, took " << tm2.realTime() << " sec";
    }
    if (idx1 != 0 && idx2 != 0 && terms.size() > 1 &&
	idx1->type() == ibis::index::RELIC &&
	idx2->type() == ibis::index::RELIC) {
	// OPTION 2 -- using relic indices to count the number of hits only
	// multiple join operators
	ibis::horometer tm1, tm2;
	ibis::bitvector64 low, high;
	bool approx2 = false;
	tm1.start();
	reinterpret_cast<const ibis::relic*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::relic*>(idx2),
		     *(terms.back()), mask, low, high);
	if (high.size() != low.size()) // more important to keep high
	    high.swap(low);
	for (int i = terms.size() - 1; i > 0 && low.cnt()>0;) {
	    -- i;
	    const char *name1 = terms[i]->getName1();
	    const ibis::column *c1 = table0->getColumn(name1);
	    const char *name2 = terms[i]->getName2();
	    const ibis::column *c2 = table0->getColumn(name2);
	    if (c1 == 0 || c2 == 0) {
		approx2 = true;
		break;
	    }

	    ibis::column::indexLock ilck1(c1, "processJoin");
	    ibis::column::indexLock ilck2(c2, "processJoin");
	    const ibis::index *ix1 = ilck1.getIndex();
	    const ibis::index *ix2 = ilck2.getIndex();
	    if (ix1 && ix2 && ix1->type() == ibis::index::RELIC &&
		ix2->type() == ibis::index::RELIC) {
		ibis::bitvector64 tmp;
		reinterpret_cast<const ibis::relic*>(ix1)->
		    estimate(*reinterpret_cast<const ibis::relic*>(ix2),
			     *(terms[i]), mask, tmp, high);
		if (tmp.cnt() > 0 && tmp.size() == low.size())
		    low &= tmp;
		else
		    low.clear();
	    }
	    else {
		approx2 = true;
		break;
	    }
	}
	int64_t cnt2 = low.cnt();
	tm1.stop();
	// OPTION 3 -- use relic indices to count the number of hits only
	// multiple join operators
	tm2.start();
	bool approx3 = false;
	reinterpret_cast<const ibis::relic*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::relic*>(idx2),
		     *(terms.back()), mask, range1, range2, low, high);
	if (high.size() != low.size()) // more important to keep high
	    high.swap(low);
	for (int i = terms.size() - 1; i > 0 && low.cnt()>0;) {
	    -- i;
	    const char *name1 = terms[i]->getName1();
	    const ibis::column *c1 = table0->getColumn(name1);
	    const char *name2 = terms[i]->getName2();
	    const ibis::column *c2 = table0->getColumn(name2);
	    if (c1 == 0 || c2 == 0) {
		approx3 = true;
		break;
	    }

	    const ibis::qRange *r1 = expr->findRange(c1->name());
	    const ibis::qRange *r2 = expr->findRange(c2->name());
	    ibis::column::indexLock ilck1(c1, "processJoin");
	    ibis::column::indexLock ilck2(c2, "processJoin");
	    const ibis::index *ix1 = ilck1.getIndex();
	    const ibis::index *ix2 = ilck2.getIndex();
	    if (ix1 && ix2 && ix1->type() == ibis::index::RELIC &&
		ix2->type() == ibis::index::RELIC) {
		ibis::bitvector64 tmp;
		reinterpret_cast<const ibis::relic*>(ix1)->
		    estimate(*reinterpret_cast<const ibis::relic*>(ix2),
			     *(terms[i]), mask, r1, r2, tmp, high);
		if (tmp.cnt() > 0 && tmp.size() == low.size())
		    low &= tmp;
		else
		    low.clear();
	    }
	    else {
		approx3 = true;
		break;
	    }
	}
	int64_t cnt3 = low.cnt();
	tm2.stop();

	ibis::util::logger lg;
	lg.buffer() << "processJoin with OPTION 2 -- basic bitmap index + "
	    "bitmap mask ";
	if (approx2)
	    lg.buffer() << "estimated to be no more than ";
	else
	    lg.buffer() << "computed ";
	lg.buffer() << cnt2 << " hits, took "
		    << tm1.realTime() << " sec\n"
		    << "processJoin with OPTION 3 -- basic bitmap index + "
	    "bitmap mask and additional range constraints";
	if (approx3)
	    lg.buffer() << " estimated to be no more than ";
	else
	    lg.buffer() << " computed ";
	lg.buffer() << cnt3 << " hits, took "
		    << tm2.realTime() << " sec";
    }
    if (idx1 != 0 && idx2 != 0 && terms.size() == 1 &&
	idx1->type() == ibis::index::BINNING &&
	idx2->type() == ibis::index::BINNING) {
	// OPTION 2 -- using binned indices to count the number of hits only
	ibis::horometer tm1, tm2;
	tm1.start();
	int64_t cnt2 = reinterpret_cast<const ibis::bin*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::bin*>(idx2),
		     *(terms.back()), mask);
	tm1.stop();
	// OPTION 3 -- use the simple bin indices to count the number of hits
	tm2.start();
	int64_t cnt3 = reinterpret_cast<const ibis::bin*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::bin*>(idx2),
		     *(terms.back()), mask, range1, range2);
	tm2.stop();

	ibis::util::logger lg;
	lg.buffer() << "processJoin with OPTION 2 -- basic binned index + "
	    "bitmap mask estimated the maximum hits to be " << cnt2
		    << ", took "
		    << tm1.realTime() << " sec\n"
		    << "processJoin with OPTION 3 -- basic binned index + "
	    "bitmap mask and additional range constraints"
	    " estimated the maximum hits to be " << cnt3
		    << ", took " << tm2.realTime() << " sec";
    }
    if (idx1 != 0 && idx2 != 0 && terms.size() > 1 &&
	idx1->type() == ibis::index::BINNING &&
	idx2->type() == ibis::index::BINNING) {
	// OPTION 2 -- using indices to count the number of hits for
	// multiple join operators
	ibis::horometer tm1, tm2;
	ibis::bitvector64 low, high;
	tm1.start();
	reinterpret_cast<const ibis::bin*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::bin*>(idx2),
		     *(terms.back()), mask, low, high);
	if (high.size() != low.size()) // more important to keep high
	    high.swap(low);
	for (int i = terms.size()-1; i > 0 && high.cnt() > 0;) {
	    -- i;
	    const char *name1 = terms[i]->getName1();
	    const ibis::column *c1 = table0->getColumn(name1);
	    const char *name2 = terms[i]->getName2();
	    const ibis::column *c2 = table0->getColumn(name2);
	    if (c1 == 0 || c2 == 0) {
		logWarning("processJoin", "either %s or %s is not a "
			   "column name", name1, name2);
	    }

	    ibis::column::indexLock ilck1(c1, "processJoin");
	    ibis::column::indexLock ilck2(c2, "processJoin");
	    const ibis::index *ix1 = ilck1.getIndex();
	    const ibis::index *ix2 = ilck2.getIndex();
	    if (ix1 && ix2 && ix1->type() == ibis::index::BINNING &&
		ix2->type() == ibis::index::BINNING) {
		ibis::bitvector64 tmp;
		reinterpret_cast<const ibis::bin*>(ix1)->
		    estimate(*reinterpret_cast<const ibis::bin*>(ix2),
			     *(terms[i]), mask, low, tmp);
		if (tmp.cnt() > 0) {
		    if (tmp.size() == high.size())
			high &= tmp;
		    else
			high &= low;
		}
		else {
		    high &= low;
		}
	    }
	    else {
		logWarning("processJoin", "either %s or %s has no binned "
			   "index", name1, name2);
	    }
	}
	int64_t cnt2 = high.cnt();
	tm1.stop();
	low.clear();
	high.clear();
	// OPTION 3 -- use binned indices to count the number of hits only,
	// multiple join operators
	tm2.start();
	reinterpret_cast<const ibis::bin*>(idx1)->
	    estimate(*reinterpret_cast<const ibis::bin*>(idx2),
		     *(terms.back()), mask, range1, range2, low, high);
	if (high.size() != low.size()) // more important to keep high
	    high.swap(low);
	for (int i = terms.size()-1; i > 0 && high.cnt() > 0;) {
	    -- i;
	    const char *name1 = terms[i]->getName1();
	    const ibis::column *c1 = table0->getColumn(name1);
	    const char *name2 = terms[i]->getName2();
	    const ibis::column *c2 = table0->getColumn(name2);
	    if (c1 == 0 || c2 == 0) {
		logWarning("processJoin", "either %s or %s is not a "
			   "column name", name1, name2);
	    }

	    const ibis::qRange *r1 = expr->findRange(c1->name());
	    const ibis::qRange *r2 = expr->findRange(c2->name());
	    ibis::column::indexLock ilck1(c1, "processJoin");
	    ibis::column::indexLock ilck2(c2, "processJoin");
	    const ibis::index *ix1 = ilck1.getIndex();
	    const ibis::index *ix2 = ilck2.getIndex();
	    if (ix1 && ix2 && ix1->type() == ibis::index::BINNING &&
		ix2->type() == ibis::index::BINNING) {
		ibis::bitvector64 tmp;
		reinterpret_cast<const ibis::bin*>(ix1)->
		    estimate(*reinterpret_cast<const ibis::bin*>(ix2),
			     *(terms[i]), mask, r1, r2, low, tmp);
		if (tmp.cnt() > 0) {
		    if (tmp.size() == high.size())
			high &= tmp;
		    else
			high &= low;
		}
		else {
		    high &= low;
		}
	    }
	    else {
		logWarning("processJoin", "either %s or %s has no binned "
			   "index", name1, name2);
	    }
	}
	int64_t cnt3 = high.cnt();
	tm2.stop();

	ibis::util::logger lg;
	lg.buffer() << "processJoin with OPTION 2 -- basic binned index + "
	    "bitmap mask estimated the maximum hits to be " << cnt2
		    << ", took " << tm1.realTime() << " sec\n"
		    << "processJoin with OPTION 3 -- basic binned index + "
	    "bitmap mask and ";
	if (range1) {
	    if (range2) {
		lg.buffer() << "two range constraints (" << *range1
			    << " and " << *range2 << ")";
	    }
	    else {
		lg.buffer() << "one range constraint (" << *range1 << ")";
	    }
	}
	else if (range2) {
	    lg.buffer() << "one range constraint (" << *range2 << ")";
	}
	else {
	    lg.buffer() << "no range constraint";
	}
	lg.buffer() << " estimated the maximum hits to be " << cnt3;
	if (terms.size() > 1)
	    lg.buffer() << " (based on " << *(terms.back()) << ")";
	lg.buffer() << ", took " << tm2.realTime() << " sec";
    }

    bool symm = false;
    { // use a block to limit the scopes of the two barrel variables
	ibis::compRange::barrel bar1, bar2;
	for (uint32_t i = 0; i < terms.size(); ++ i) {
	    bar1.recordVariable(terms[i]->getName1());
	    bar1.recordVariable(terms[i]->getRange());
	    bar2.recordVariable(terms[i]->getName2());
	}
	symm = bar1.equivalent(bar2);
    }

    // OPTION 4 -- the intended main option that combines the index
    // operations with brute-fore scans.  Since it uses a large bitvector64
    // as a mask, we need to make sure it can be safely generated in
    // memory.
    {
	double cf = ibis::bitvector::clusteringFactor
	    (mask.size(), mask.cnt(), mask.bytes());
	uint64_t mb = mask.cnt();
	double bv64size = 8*ibis::bitvector64::markovSize
	    (npairs, mb*mb, cf);
	if (bv64size > 2.0*ibis::fileManager::bytesFree() ||
	    bv64size > ibis::fileManager::bytesFree() +
	    ibis::fileManager::bytesInUse()) {
	    logWarning("processJoin", "the solution vector for a join of "
		       "%lu x %lu (out of %lu x %lu) is expected to take %g "
		       "bytes and can not be fit into available memory",
		       static_cast<long unsigned>(mask.cnt()),
		       static_cast<long unsigned>(mask.cnt()),
		       static_cast<long unsigned>(mask.size()),
		       static_cast<long unsigned>(mask.size()),
		       bv64size);
	    return cnt;
	}
    }

    timer.start();
    uint64_t estimated = 0;
    ibis::bitvector64 surepairs, iffypairs;
    if (terms.size() == 1) {
	if (idx1 != 0 && idx2 != 0 &&
	    idx1->type() == ibis::index::RELIC &&
	    idx2->type() == ibis::index::RELIC) {
	    reinterpret_cast<const ibis::relic*>(idx1)->
		estimate(*reinterpret_cast<const ibis::relic*>(idx2),
			 *(terms.back()), mask, range1, range2,
			 surepairs, iffypairs);
	}
	else if (idx1 != 0 && idx2 != 0 &&
		 idx1->type() == ibis::index::BINNING &&
		 idx2->type() == ibis::index::BINNING) {
	    if (symm)
		reinterpret_cast<const ibis::bin*>(idx1)->estimate
		    (*(terms.back()), mask, range1, range2,
		     surepairs, iffypairs);
	    else
		reinterpret_cast<const ibis::bin*>(idx1)->estimate
		    (*reinterpret_cast<const ibis::bin*>(idx2),
		     *(terms.back()), mask, range1, range2,
		     surepairs, iffypairs);
	}
	else {
	    surepairs.set(0, npairs);
	    iffypairs.set(1, npairs);
	}

	if (iffypairs.size() != npairs)
	    iffypairs.set(0, npairs);
	if (surepairs.size() != npairs)
	    surepairs.set(0, npairs);
	estimated = iffypairs.cnt();
	if (surepairs.cnt() > 0 || iffypairs.cnt() > 0) {
	    ibis::bitvector64 tmp;
	    ibis::outerProduct(mask, mask, tmp);
	    //    std::cout << "TEMP surepairs.size() = " << surepairs.size()
	    //	      << " surepairs.cnt() = " << surepairs.cnt() << std::endl;
	    //    std::cout << "TEMP iffypairs.size() = " << iffypairs.size()
	    //	      << " iffypairs.cnt() = " << iffypairs.cnt() << std::endl;
	    //    std::cout << "TEMP tmp.size() = " << tmp.size()
	    //	      << ", tmp.cnt() = " << tmp.cnt() << std::endl;
	    surepairs &= tmp;
	    iffypairs &= tmp;
	    iffypairs -= surepairs;
#if defined(DEBUG) && DEBUG > 2
	    if (surepairs.cnt() > 0) { // verify the pairs in surepairs
		int64_t ct1 = table0->evaluateJoin(*(terms.back()),
						   surepairs, tmp);
		if (ct1 > 0 && tmp.size() == surepairs.size())
		    tmp ^= surepairs;
		if (tmp.cnt() != 0) {
		    std::ostringstream ostr;
		    ostr << tmp.cnt();
		    logWarning("processJoin", "some (%s) surepairs are "
			       "not correct", ostr.str().c_str());
		    const unsigned nrows = table0->nRows();
		    ibis::util::logger lg;
		    for (ibis::bitvector64::indexSet ix =
			     tmp.firstIndexSet();
			 ix.nIndices() > 0; ++ ix) {
			const ibis::bitvector64::word_t *ind = ix.indices();
			if (ix.isRange()) {
			    for (ibis::bitvector64::word_t i = *ind;
				 i < ind[1]; ++ i)
				lg.buffer() << i/nrows << ",\t" << i%nrows
					    << "\n";
			}
			else {
			    for (uint32_t i = 0; i < ix.nIndices(); ++ i)
				lg.buffer() << ind[i]/nrows << ",\t"
					    << ind[i]%nrows << "\n";
			}
		    }
		}
	    }
#endif
	    if (iffypairs.cnt() <
		static_cast<uint64_t>(mask.cnt())*mask.cnt()) {
		int64_t ct2 = table0->evaluateJoin(*(terms.back()),
						   iffypairs, tmp);
		if (ct2 > 0 && tmp.size() == surepairs.size())
		    surepairs |= tmp;
#if defined(DEBUG) && DEBUG + 0 > 1
		ct2 = table0->evaluateJoin(*(terms.back()), mask, iffypairs);
		if (ct2 > 0 && iffypairs.size() == surepairs.size())
		    iffypairs ^= surepairs;
		if (iffypairs.cnt() == 0) {
		    logMessage("processJoin", "bruteforce scan produced "
			       "the same results as indexed scan");
		}
		else {
		    std::ostringstream ostr;
		    ostr << iffypairs.cnt();
		    logWarning("processJoin", "bruteforce scan produced %s "
			       "different results from the indexed scan",
			       ostr.str().c_str());
		    const unsigned nrows = table0->nRows();
		    ibis::util::logger lg;
		    for (ibis::bitvector64::indexSet ix =
			     iffypairs.firstIndexSet();
			 ix.nIndices() > 0; ++ ix) {
			const ibis::bitvector64::word_t *ind = ix.indices();
			if (ix.isRange()) {
			    for (ibis::bitvector64::word_t i = *ind;
				 i < ind[1]; ++ i)
				lg.buffer() << i/nrows << ",\t" << i%nrows
					    << "\n";
			}
			else {
			    for (uint32_t i = 0; i < ix.nIndices(); ++ i)
				lg.buffer() << ind[i]/nrows << ",\t"
					    << ind[i]%nrows << "\n";
			}
		    }
		}
#endif
	    }
	    else {
		table0->evaluateJoin(*(terms.back()), mask, surepairs);
	    }
	}
    }
    else { // more than one join term to be processed
	if (idx1 != 0 && idx2 != 0 &&
	    idx1->type() == ibis::index::BINNING &&
	    idx2->type() == ibis::index::BINNING) {
	    if (symm)
		reinterpret_cast<const ibis::bin*>(idx1)->estimate
		    (*(terms.back()), mask, range1, range2,
		     surepairs, iffypairs);
	    else
		reinterpret_cast<const ibis::bin*>(idx1)->estimate
		    (*reinterpret_cast<const ibis::bin*>(idx2),
		     *(terms.back()), mask, range1, range2,
		     surepairs, iffypairs);
	}
	else if (idx1 != 0 && idx2 != 0 &&
		 idx1->type() == ibis::index::RELIC &&
		 idx2->type() == ibis::index::RELIC) {
	    reinterpret_cast<const ibis::relic*>(idx1)->
		estimate(*reinterpret_cast<const ibis::relic*>(idx2),
			 *(terms.back()), mask, range1, range2,
			 surepairs, iffypairs);
	}
	else {
	    surepairs.set(0, npairs);
	    ibis::outerProduct(mask, mask, iffypairs);
	}

	if (iffypairs.size() != npairs)
	    iffypairs.set(0, npairs);
	if (surepairs.size() != npairs)
	    surepairs.set(0, npairs);
	iffypairs |= surepairs;
	estimated = iffypairs.cnt();
	if (iffypairs.cnt() < static_cast<uint64_t>(mask.cnt())*mask.cnt()) {
	    if (iffypairs.cnt() == surepairs.cnt())
		// the last term has been evaluated accurately, remove it
		terms.resize(terms.size() - 1);
	    int64_t ct4 = table0->evaluateJoin(terms, iffypairs, surepairs);
	    if (ct4 < 0) {
		logWarning("processJoin",
			   "evaluateJoin failed with error code %ld",
			   static_cast<long int>(ct4));
	    }
#if defined(DEBUG) && DEBUG + 0 > 1
	    ct4 = table0->evaluateJoin(terms, mask, iffypairs);
	    if (ct4 > 0 && iffypairs.size() == surepairs.size())
		iffypairs ^= surepairs;
	    if (iffypairs.cnt() == 0) {
		logMessage("processJoin", "verified the indexed scan "
			   "results with bruteforce scan");
	    }
	    else {
		std::ostringstream ostr;
		ostr << iffypairs.cnt();
		logWarning("processJoin", "bruteforce scan produced %s "
			   "different results from the indexed scan",
			   ostr.str().c_str());
		const unsigned nrows = table0->nRows();
		ibis::util::logger lg;
		for (ibis::bitvector64::indexSet ix =
			 iffypairs.firstIndexSet();
		     ix.nIndices() > 0; ++ ix) {
		    const ibis::bitvector64::word_t *ind = ix.indices();
		    if (ix.isRange()) {
			for (ibis::bitvector64::word_t i = *ind;
			     i < ind[1]; ++ i)
			    lg.buffer() << i/nrows << ",\t" << i%nrows << "\n";
		    }
		    else {
			for (uint32_t i = 0; i < ix.nIndices(); ++ i)
			    lg.buffer() << ind[i]/nrows << ",\t"
					<< ind[i]%nrows << "\n";
		    }
		}
	    }
#endif
	}
	else {
	    table0->evaluateJoin(terms, mask, surepairs);
	}
    }

    ret = surepairs.cnt();
    timer.stop();
    //     logMessage("processJoin", "OPTION 4 -- indexed join computes %lld "
    // 	       "hits in %g seconds", ret, timer.realTime());
    //     logMessage("processJoin", "OPTION 4 -- indexed join computes %lld "
    // 	       "hits in %g seconds", ret, timer.realTime());
    LOGGER(ibis::gVerbose >= 0)
	<< "processJoin with OPTION 4 -- index scan (estimated <= "
	<< estimated << ") followed by pair-masked loop join computed "
	<< ret << (ret > 1 ? " hits" : " hit") << ", took "
	<< timer.realTime() << " sec";

    if (cnt == ret) {
	if (ibis::gVerbose > 4)
	    logMessage("processJoin", "merge join algorithm produced the "
		       "same number of hits as the indexed/sequential scan");
    }
    else {
	std::ostringstream o2;
	o2 << cnt << " hit" << (cnt>1?"s":"") << " rather than " << ret
	   << " as produced from the indexed/sequential scan";
	logWarning("processJoin", "merge join algorithm produced %s",
		   o2.str().c_str());
    }

    if (ibis::gVerbose > 0) {
	outstr << "), got " << ret << (ret > 1 ? " hits" : " hit");
	logMessage("processJoin", "%s, used %g sec(CPU) %g sec(elapsed)",
		   outstr.str().c_str(), timer.CPUTime(), timer.realTime());
    }
    return ret;
} // ibis::query::processJoin

// The merge sort join algorithm.
int64_t ibis::query::sortJoin(const ibis::rangeJoin& cmp,
			      const ibis::bitvector& mask) const {
    int64_t cnt = 0;
    if (cmp.getRange() == 0)
	cnt = sortEquiJoin(cmp, mask);
    else if (cmp.getRange()->termType() == ibis::compRange::NUMBER) {
	const double delta = fabs(cmp.getRange()->eval());
	if (delta > 0)
	    cnt = sortRangeJoin(cmp, mask);
	else
	    cnt = sortEquiJoin(cmp, mask);
    }
    else {
	ibis::compRange::barrel bar(cmp.getRange());
	if (bar.size() == 0) {
	    const double delta = fabs(cmp.getRange()->eval());
	    if (delta > 0)
		cnt = sortRangeJoin(cmp, mask);
	    else
		cnt = sortEquiJoin(cmp, mask);
	}
	else {
	    cnt = table0->evaluateJoin(cmp, mask);
	}
    }
    return cnt;
} // ibis::query::sortJoin

int64_t
ibis::query::sortJoin(const std::vector<const ibis::rangeJoin*>& terms,
		      const ibis::bitvector& mask) const {
    if (terms.size() > 1) {
	if (myDir == 0) {
	    logWarning("sortJoin", "unable to create a directory to store "
		       "temporary files needed for the sort-merge join "
		       "algorithm.  Use loop join instead.");
	    return table0->evaluateJoin(terms, mask);
	}

	int64_t cnt = mask.cnt();
	for (uint32_t i = 0; i < terms.size() && cnt > 0; ++ i) {
	    std::string pairfile = myDir;
	    pairfile += terms[i]->getName1();
	    pairfile += '-';
	    pairfile += terms[i]->getName2();
	    pairfile += ".pairs";
	    if (terms[i]->getRange() == 0) {
		sortEquiJoin(*(terms[i]), mask, pairfile.c_str());
	    }
	    else if (terms[i]->getRange()->termType() ==
		     ibis::compRange::NUMBER) {
		const double delta = fabs(terms[i]->getRange()->eval());
		if (delta > 0)
		    sortRangeJoin(*(terms[i]), mask, pairfile.c_str());
		else
		    sortEquiJoin(*(terms[i]), mask, pairfile.c_str());
	    }
	    else {
		ibis::compRange::barrel bar(terms[i]->getRange());
		if (bar.size() == 0) {
		    const double delta = fabs(terms[i]->getRange()->eval());
		    if (delta > 0)
			sortRangeJoin(*(terms[i]), mask, pairfile.c_str());
		    else
			sortEquiJoin(*(terms[i]), mask, pairfile.c_str());
		}
		else {
		    table0->evaluateJoin(*(terms[i]), mask,
					 pairfile.c_str());
		}
	    }

	    // sort the newly generated pairs
	    orderPairs(pairfile.c_str());
	    // merge the sorted pairs with the existing list
	    cnt = mergePairs(pairfile.c_str());
	}
	return cnt;
    }
    else if (terms.size() == 1) {
	return sortJoin(*(terms.back()), mask);
    }
    else {
	return 0;
    }
} // ibis::query::sortJoin

/// Assume the two input arrays are sorted in ascending order, count the
/// number of elements that match.  Note that both template arguments
/// should be elemental types or they must support operators == and < with
/// mixed types.
template <typename T1, typename T2>
int64_t ibis::query::countEqualPairs(const array_t<T1>& val1,
				     const array_t<T2>& val2) const {
    int64_t cnt = 0;
    uint32_t i1 = 0, i2 = 0;
    const uint32_t n1 = val1.size();
    const uint32_t n2 = val2.size();
    while (i1 < n1 && i2 < n2) {
	uint32_t j1, j2;
	if (val1[i1] < val2[i2]) { // move i1 to catch up
	    for (++ i1; i1 < n1 && val1[i1] < val2[i2]; ++ i1);
	}
	if (val2[i2] < val1[i1]) { // move i2 to catch up
	    for (++ i2; i2 < n2 && val2[i2] < val1[i1]; ++ i2);
	}
	if (i1 < n1 && i2 < n2 && val1[i1] == val2[i2]) {
	    // found two equal values
	    // next, find out how many values are equal
	    for (j1 = i1+1; j1 < n1 && val1[j1] == val1[i1]; ++ j1);
	    for (j2 = i2+1; j2 < n2 && val2[i2] == val2[j2]; ++ j2);
#ifdef DEBUG
	    LOGGER(ibis::gVerbose >= 0)
		<< "DEBUG: query::countEqualPairs found "
		<< "val1[" << i1 << ":" << j1 << "] (" << val1[i1]
		<< ") equals to val2[" << i2 << ":" << j2
		<< "] (" << val2[i2] << ")";
#endif
	    cnt += (j1 - i1) * (j2 - i2);
	    i1 = j1;
	    i2 = j2;
	}
    } // while (i1 < n1 && i2 < n2)
    return cnt;
} // ibis::query::countEqualPairs

// two specialization for comparing signed and unsigned integers.
template <>
int64_t ibis::query::countEqualPairs(const array_t<int32_t>& val1,
				     const array_t<uint32_t>& val2) const {
    int64_t cnt = 0;
    uint32_t i1 = val1.find(val2.front()); // position of the first value >= 0
    uint32_t i2 = 0;
    const uint32_t n1 = val1.size();
    const uint32_t n2 = val2.find(val1.back()+1U);
    while (i1 < n1 && i2 < n2) {
	uint32_t j1, j2;
	if (static_cast<unsigned>(val1[i1]) < val2[i2]) {
	    // move i1 to catch up
	    for (++ i1;
		 i1 < n1 && static_cast<unsigned>(val1[i1]) < val2[i2];
		 ++ i1);
	}
	if (val2[i2] < static_cast<unsigned>(val1[i1])) {
	    // move i2 to catch up
	    for (++ i2;
		 i2 < n2 && val2[i2] < static_cast<unsigned>(val1[i1]);
		 ++ i2);
	}
	if (i1 < n1 && i2 < n2 &&
	    static_cast<unsigned>(val1[i1]) == val2[i2]) {
	    // found two equal values
	    // next, find out how many values are equal
	    for (j1 = i1+1; j1 < n1 && val1[j1] == val1[i1]; ++ j1);
	    for (j2 = i2+1; j2 < n2 && val2[i2] == val2[j2]; ++ j2);
	    cnt += (j1 - i1) * (j2 - i2);
	    i1 = j1;
	    i2 = j2;
	}
    } // while (i1 < n1 && i2 < n2)
    return cnt;
} // ibis::query::countEqualPairs

template <>
int64_t ibis::query::countEqualPairs(const array_t<uint32_t>& val1,
				     const array_t<int32_t>& val2) const {
    int64_t cnt = 0;
    uint32_t i1 = 0, i2 = val2.find(val1.front());
    const uint32_t n1 = val1.find(val2.back()+1U);
    const uint32_t n2 = val2.size();
    while (i1 < n1 && i2 < n2) {
	uint32_t j1, j2;
	if (val1[i1] < static_cast<unsigned>(val2[i2])) {
	    // move i1 to catch up
	    for (++ i1;
		 i1 < n1 && val1[i1] < static_cast<unsigned>(val2[i2]);
		 ++ i1);
	}
	if (static_cast<unsigned>(val2[i2]) < val1[i1]) {
	    // move i2 to catch up
	    for (++ i2;
		 i2 < n2 && static_cast<unsigned>(val2[i2]) < val1[i1];
		 ++ i2);
	}
	if (i1 < n1 && i2 < n2 &&
	    val1[i1] == static_cast<unsigned>(val2[i2])) {
	    // found two equal values
	    // next, find out how many values are equal
	    for (j1 = i1+1; j1 < n1 && val1[j1] == val1[i1]; ++ j1);
	    for (j2 = i2+1; j2 < n2 && val2[i2] == val2[j2]; ++ j2);
#ifdef DEBUG
	    LOGGER(ibis::gVerbose >= 0)
		<< "DEBUG: query::countEqualPairs found "
		<< "val1[" << i1 << ":" << j1 << "] (" << val1[i1]
		<< ") equals to val2[" << i2 << ":" << j2
		<< "] (" << val2[i2] << ")";
#endif
	    cnt += (j1 - i1) * (j2 - i2);
	    i1 = j1;
	    i2 = j2;
	}
    } // while (i1 < n1 && i2 < n2)
    return cnt;
} // ibis::query::countEqualPairs

/// Assume the two input arrays are sorted in ascending order, count the
/// number of elements that are with delta of each other.  Note that both
/// template arguments should be elemental types or they must support
/// operators -, +, == and < with mixed types.
template <typename T1, typename T2>
int64_t ibis::query::countDeltaPairs(const array_t<T1>& val1,
				     const array_t<T2>& val2,
				     const T1& delta) const {
    if (delta <= 0)
	return countEqualPairs(val1, val2);

    int64_t cnt = 0;
    uint32_t i1 = 0, i2 = 0;
    const uint32_t n1 = val1.size();
    for (uint32_t i = 0; i < val2.size() && i1 < n1; ++ i) {
	const T1 hi = static_cast<T1>(val2[i] + delta);
	// pressume integer underflow, set it to 0
	const T1 lo = (static_cast<T1>(val2[i] - delta)<hi ?
		       static_cast<T1>(val2[i] - delta) : 0);
	// move i1 to catch up with lo
	while (i1 < n1 && val1[i1] < lo)
	    ++ i1;
	// move i2 to catch up with hi
	if (i1 > i2)
	    i2 = i1;
	while (i2 < n1 && val1[i2] <= hi)
	    ++ i2;
	cnt += i2 - i1;
    } // for ..
#ifdef DEBUG
    ibis::util::logger lg;
    lg.buffer() << "DEBUG: countDeltaPairs val1=[";
    for (uint32_t ii = 0; ii < val1.size(); ++ ii)
	lg.buffer() << val1[ii] << ' ';
    lg.buffer() << "]\n" << "DEBUG: countDeltaPairs val2=[";
    for (uint32_t ii = 0; ii < val2.size(); ++ ii)
	lg.buffer() << val2[ii] << ' ';
    lg.buffer() << "]\nDEBUG: cnt=" << cnt;
#endif
    return cnt;
} // ibis::query::countDeltaPairs

template <>
int64_t ibis::query::countDeltaPairs(const array_t<uint32_t>& val1,
				     const array_t<int32_t>& val2,
				     const uint32_t& delta) const {
    int64_t cnt = 0;
    uint32_t i1 = 0, i2 = 0;
    const uint32_t n1 = val1.find(val2.back()+1U+delta);
    for (uint32_t i = val2.find(static_cast<int>(val1.front()-delta));
	 i < val2.size() && i1 < n1; ++ i) {
	const unsigned lo = (static_cast<unsigned>(val2[i]) >= delta ?
			     val2[i] - delta : 0);
	const unsigned hi = val2[i] + delta;
	// move i1 to catch up with lo
	while (i1 < n1 && val1[i1] < lo)
	    ++ i1;
	// move i2 to catch up with hi
	if (i1 > i2)
	    i2 = i1;
	while (i2 < n1 && val1[i2] <= hi)
	    ++ i2;
	cnt += i2 - i1;
#ifdef DEBUG
	LOGGER(ibis::gVerbose-1)
	    << "DEBUG: query::countDeltaPairs found "
	    << "val2[" << i << "] (" << val2[i]
	    << ") in the range of val1[" << i1 << ":" << i2
	    << "] (" << val1[i1] << " -- " << val1[i2] << ")";
#endif
    } // for ..
    return cnt;
} // ibis::query::countDeltaPairs

template <>
int64_t ibis::query::countDeltaPairs(const array_t<int32_t>& val1,
				     const array_t<uint32_t>& val2,
				     const int32_t& delta) const {
    if (delta <= 0)
	return countEqualPairs(val1, val2);

    int64_t cnt = 0;
    uint32_t i1 = val1.find(val2.front()-delta);
    uint32_t i2 = 0;
    const uint32_t n1 = val1.size();
    const uint32_t n2 = val2.find(UINT_MAX);
    for (uint32_t i = 0; i < n2 && i1 < n1; ++ i) {
	const int lo = val2[i] - delta;
	const int hi = val2[i] + delta;
	// move i1 to catch up with lo
	while (i1 < n1 && val1[i1] < lo)
	    ++ i1;
	// move i2 to catch up with hi
	if (i1 > i2)
	    i2 = i1;
	while (i2 < n1 && val1[i2] <= hi)
	    ++ i2;
	cnt += i2 - i1;
    } // for ..
    return cnt;
} // ibis::query::countDeltaPairs

template <typename T1, typename T2>
int64_t ibis::query::recordEqualPairs(const array_t<T1>& val1,
				      const array_t<T2>& val2,
				      const array_t<uint32_t>& ind1,
				      const array_t<uint32_t>& ind2,
				      const char* filename) const {
    if (filename == 0 || *filename == 0)
	return countEqualPairs(val1, val2);
    int fdes = UnixOpen(filename, OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	logWarning("recordEqualPairs",
		   "failed to open file \"%s\" for writing", filename);
	return countEqualPairs(val1, val2);
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int64_t cnt = 0;
    uint32_t idbuf[2];
    const uint32_t idsize = sizeof(idbuf);
    uint32_t i1 = 0, i2 = 0;
    const uint32_t n1 = val1.size();
    const uint32_t n2 = val2.size();
    while (i1 < n1 && i2 < n2) {
	uint32_t j1, j2;
	if (val1[i1] < val2[i2]) { // move i1 to catch up
	    for (++ i1; i1 < n1 && val1[i1] < val2[i2]; ++ i1);
	}
	if (val2[i2] < val1[i1]) { // move i2 to catch up
	    for (++ i2; i2 < n2 && val2[i2] < val1[i1]; ++ i2);
	}
	if (i1 < n1 && i2 < n2 && val1[i1] == val2[i2]) {
	    // found two equal values
	    // next, find out how many values are equal
	    for (j1 = i1+1; j1 < n1 && val1[j1] == val1[i1]; ++ j1);
	    for (j2 = i2+1; j2 < n2 && val2[i2] == val2[j2]; ++ j2);
	    if (ind1.size() == val1.size() && ind2.size() == val2.size()) {
		for (uint32_t ii = i1; ii < j1; ++ ii) {
		    idbuf[0] = ind1[ii];
		    for (uint32_t jj = i2; jj < j2; ++ jj) {
			idbuf[1] = ind2[jj];
			UnixWrite(fdes, idbuf, idsize);
		    }
		}
	    }
	    else {
		for (idbuf[0] = i1; idbuf[0] < j1; ++ idbuf[0])
		    for (idbuf[1] = i2; idbuf[1] < j2; ++ idbuf[1])
			UnixWrite(fdes, idbuf, idsize);
	    }
#ifdef DEBUG
	    LOGGER(ibis::gVerbose >= 0)
		<< "DEBUG: query::recordEqualPairs found "
		<< "val1[" << i1 << ":" << j1 << "] (" << val1[i1]
		<< ") equals to val2[" << i2 << ":" << j2
		<< "] (" << val2[i2] << ")";
#endif
	    cnt += (j1 - i1) * (j2 - i2);
	    i1 = j1;
	    i2 = j2;
	}
    } // while (i1 < n1 && i2 < n2)
    UnixClose(fdes);
    return cnt;
} // ibis::query::recordEqualPairs

template <>
int64_t ibis::query::recordEqualPairs(const array_t<uint32_t>& val1,
				      const array_t<int32_t>& val2,
				      const array_t<uint32_t>& ind1,
				      const array_t<uint32_t>& ind2,
				      const char* filename) const {
    if (filename == 0 || *filename == 0)
	return countEqualPairs(val1, val2);
    int fdes = UnixOpen(filename, OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	logWarning("recordEqualPairs",
		   "failed to open file \"%s\" for writing", filename);
	return countEqualPairs(val1, val2);
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int64_t cnt = 0;
    uint32_t idbuf[2];
    const uint32_t idsize = sizeof(idbuf);
    uint32_t i1 = 0, i2 = val2.find(val1.front());
    const uint32_t n1 = val1.find(val2.back()+1U);
    const uint32_t n2 = val2.size();
    while (i1 < n1 && i2 < n2) {
	uint32_t j1, j2;
	if (val1[i1] < (unsigned) val2[i2]) { // move i1 to catch up
	    for (++ i1; i1 < n1 && val1[i1] < (unsigned) val2[i2]; ++ i1);
	}
	if ((unsigned) val2[i2] < val1[i1]) { // move i2 to catch up
	    for (++ i2; i2 < n2 && (unsigned) val2[i2] < val1[i1]; ++ i2);
	}
	if (i1 < n1 && i2 < n2 && val1[i1] == (unsigned) val2[i2]) {
	    // found two equal values
	    // next, find out how many values are equal
	    for (j1 = i1+1; j1 < n1 && val1[j1] == val1[i1]; ++ j1);
	    for (j2 = i2+1; j2 < n2 && val2[i2] == val2[j2]; ++ j2);
	    if (ind1.size() == val1.size() && ind2.size() == val2.size()) {
		for (uint32_t ii = i1; ii < j1; ++ ii) {
		    idbuf[0] = ind1[ii];
		    for (uint32_t jj = i2; jj < j2; ++ jj) {
			idbuf[1] = ind2[jj];
			UnixWrite(fdes, idbuf, idsize);
		    }
		}
	    }
	    else {
		for (idbuf[0] = i1; idbuf[0] < j1; ++ idbuf[0])
		    for (idbuf[1] = i2; idbuf[1] < j2; ++ idbuf[1])
			UnixWrite(fdes, idbuf, idsize);
	    }
	    cnt += (j1 - i1) * (j2 - i2);
	    i1 = j1;
	    i2 = j2;
	}
    } // while (i1 < n1 && i2 < n2)
    UnixClose(fdes);
    return cnt;
} // ibis::query::recordEqualPairs

template <>
int64_t ibis::query::recordEqualPairs(const array_t<int32_t>& val1,
				      const array_t<uint32_t>& val2,
				      const array_t<uint32_t>& ind1,
				      const array_t<uint32_t>& ind2,
				      const char* filename) const {
    if (filename == 0 || *filename == 0)
	return countEqualPairs(val1, val2);
    int fdes = UnixOpen(filename, OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	logWarning("recordEqualPairs",
		   "failed to open file \"%s\" for writing", filename);
	return countEqualPairs(val1, val2);
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int64_t cnt = 0;
    uint32_t idbuf[2];
    const uint32_t idsize = sizeof(idbuf);
    uint32_t i1 = val1.find(val2.front()), i2 = 0;
    const uint32_t n1 = val1.size();
    const uint32_t n2 = val2.find(val1.back()+1U);
    while (i1 < n1 && i2 < n2) {
	uint32_t j1, j2;
	if (static_cast<unsigned>(val1[i1]) < val2[i2]) {
	    // move i1 to catch up
	    for (++ i1;
		 i1 < n1 && static_cast<unsigned>(val1[i1]) < val2[i2];
		 ++ i1);
	}
	if (val2[i2] < static_cast<unsigned>(val1[i1])) {
	    // move i2 to catch up
	    for (++ i2;
		 i2 < n2 && val2[i2] < static_cast<unsigned>(val1[i1]);
		 ++ i2);
	}
	if (i1 < n1 && i2 < n2 &&
	    static_cast<unsigned>(val1[i1]) == val2[i2]) {
	    // found two equal values
	    // next, find out how many values are equal
	    for (j1 = i1+1; j1 < n1 && val1[j1] == val1[i1]; ++ j1);
	    for (j2 = i2+1; j2 < n2 && val2[i2] == val2[j2]; ++ j2);
	    if (ind1.size() == val1.size() && ind2.size() == val2.size()) {
		for (uint32_t ii = i1; ii < j1; ++ ii) {
		    idbuf[0] = ind1[ii];
		    for (uint32_t jj = i2; jj < j2; ++ jj) {
			idbuf[1] = ind2[jj];
			UnixWrite(fdes, idbuf, idsize);
		    }
		}
	    }
	    else {
		for (idbuf[0] = i1; idbuf[0] < i2; ++ idbuf[0])
		    for (idbuf[1] = j1; idbuf[1] < j2; ++ idbuf[1])
			UnixWrite(fdes, idbuf, idsize);
	    }
	    cnt += (j1 - i1) * (j2 - i2);
	    i1 = j1;
	    i2 = j2;
	}
    } // while (i1 < n1 && i2 < n2)
    UnixClose(fdes);
    return cnt;
} // ibis::query::recordEqualPairs

template <typename T1, typename T2>
int64_t ibis::query::recordDeltaPairs(const array_t<T1>& val1,
				      const array_t<T2>& val2,
				      const array_t<uint32_t>& ind1,
				      const array_t<uint32_t>& ind2,
				      const T1& delta,
				      const char* filename) const {
    if (filename == 0 || *filename == 0)
	return countDeltaPairs(val1, val2, delta);
    if (delta <= 0)
	return recordEqualPairs(val1, val2, ind1, ind2, filename);
    int fdes = UnixOpen(filename, OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	logWarning("recordDeltaPairs",
		   "failed to open file \"%s\" for writing", filename);
	return countDeltaPairs(val1, val2, delta);
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

#ifdef DEBUG
    for (uint32_t i = 0; i < ind1.size(); ++ i)
	if (ind1[i] > table0->nRows())
	    logWarning("recordDeltaPairs", "ind1[%lu] = %lu is out of "
		       "range (shoud be < %lu)", static_cast<long unsigned>(i),
		       static_cast<long unsigned>(ind1[i]),
		       static_cast<long unsigned>(table0->nRows()));
    for (uint32_t i = 0; i < ind2.size(); ++ i)
	if (ind2[i] > table0->nRows())
	    logWarning("recordDeltaPairs", "ind2[%lu] = %lu is out of "
		       "range (shoud be < %lu)", static_cast<long unsigned>(i),
		       static_cast<long unsigned>(ind2[i]),
		       static_cast<long unsigned>(table0->nRows()));
#endif
    int64_t cnt = 0;
    uint32_t idbuf[2];
    const uint32_t idsize = sizeof(idbuf);
    uint32_t i1 = 0, i2 = 0;
    const uint32_t n1 = val1.size();
    for (uint32_t i = 0; i < val2.size() && i1 < n1; ++ i) {
	const T1 hi = static_cast<T1>(val2[i] + delta);
	// presume integer underflow, set it to 0
	const T1 lo = (static_cast<T1>(val2[i] - delta) < hi ?
		       static_cast<T1>(val2[i] - delta) : 0);
	// move i1 to catch up with lo
	while (i1 < n1 && val1[i1] < lo)
	    ++ i1;
	// move i2 to catch up with hi
	if (i1 > i2)
	    i2 = i1;
	while (i2 < n1 && val1[i2] <= hi)
	    ++ i2;

	idbuf[1] = (ind2.size() == val2.size() ? ind2[i] : i);
	if (ind1.size() == val1.size()) {
	    for (uint32_t jj = i1; jj < i2; ++ jj) {
		idbuf[0] = ind1[jj];
		UnixWrite(fdes, idbuf, idsize);
#ifdef DEBUG
		if (idbuf[0] != ind1[jj] || idbuf[1] !=
		    (ind2.size() == val2.size() ? ind2[i] : i) ||
		    idbuf[0] >= table0->nRows() ||
		    idbuf[1] >= table0->nRows()) {
		    logWarning("recordDeltaPairs", "idbuf (%lu, %lu) differs "
			       "from expected (%lu, %lu) or is out of range",
			       static_cast<long unsigned>(idbuf[0]),
			       static_cast<long unsigned>(idbuf[1]),
			       static_cast<long unsigned>(ind1[jj]),
			       static_cast<long unsigned>
			       (ind2.size() == val2.size() ? ind2[i] : i));
		}
#endif
	    }
	}
	else {
	    for (idbuf[0] = i1; idbuf[0] < i2 && idbuf[0] < n1; ++ idbuf[0])
		UnixWrite(fdes, idbuf, idsize);
	}
	cnt += i2 - i1;
    } // for ..
    UnixClose(fdes);
#ifdef DEBUG
    ibis::util::logger lg(4);
    lg.buffer() << "DEBUG: recordDeltaPairs val1=[";
    for (uint32_t ii = 0; ii < val1.size(); ++ ii)
	lg.buffer() << val1[ii] << ' ';
    lg.buffer() << "]\n";
    if (ind1.size() == val1.size()) {
	lg.buffer() << "DEBUG: recordDeltaPairs ind1=[";
	for (uint32_t ii = 0; ii < ind1.size(); ++ ii)
	    lg.buffer() << ind1[ii] << ' ';
	lg.buffer() << "]\n";
    }
    lg.buffer() << "DEBUG: recordDeltaPairs val2=[";
    for (uint32_t ii = 0; ii < val2.size(); ++ ii)
	lg.buffer() << val2[ii] << ' ';
    lg.buffer() << "]\n";
    if (ind2.size() == val2.size()) {
	lg.buffer() << "DEBUG: recordDeltaPairs ind2=[";
	for (uint32_t ii = 0; ii < ind2.size(); ++ ii)
	    lg.buffer() << ind2[ii] << ' ';
	lg.buffer() << "]\n";
    }
    lg.buffer() << "DEBUG: cnt=" << cnt;
#endif
    return cnt;
} // ibis::query::recordDeltaPairs

template <>
int64_t ibis::query::recordDeltaPairs(const array_t<uint32_t>& val1,
				      const array_t<int32_t>& val2,
				      const array_t<uint32_t>& ind1,
				      const array_t<uint32_t>& ind2,
				      const uint32_t& delta,
				      const char* filename) const {
    if (filename == 0 || *filename == 0)
	return countDeltaPairs(val1, val2, delta);
    if (delta <= 0)
	return recordEqualPairs(val1, val2, ind1, ind2, filename);
    int fdes = UnixOpen(filename, OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	logWarning("recordDeltaPairs",
		   "failed to open file \"%s\" for writing", filename);
	return countDeltaPairs(val1, val2, delta);
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int64_t cnt = 0;
    uint32_t idbuf[2];
    const uint32_t idsize = sizeof(idbuf);
    uint32_t i1 = 0, i2 = 0;
    const uint32_t n1 = val1.size();
    for (uint32_t i = val2.find(static_cast<int>(val1.front()-delta));
	 i < val2.size() && i1 < n1; ++ i) {
	const unsigned lo = static_cast<unsigned>
	    (val2[i] > static_cast<int>(delta) ? val2[i] - delta : 0);
	const unsigned hi = static_cast<unsigned>(val2[i] + delta);
	// move i1 to catch up with lo
	while (i1 < n1 && val1[i1] < lo)
	    ++ i1;
	// move i2 to catch up with hi
	if (i1 > i2)
	    i2 = i1;
	while (i2 < n1 && val1[i2] <= hi)
	    ++ i2;
	idbuf[1] = (ind2.size() == val2.size() ? ind2[i] : i);
	if (ind1.size() == val1.size()) {
	    for (uint32_t jj = i1; jj < i2; ++ jj) {
		idbuf[0] = ind1[jj];
		UnixWrite(fdes, idbuf, idsize);
	    }
	}
	else {
	    for (idbuf[0] = i1; idbuf[0] < i2 && idbuf[0] < n1; ++ idbuf[0])
		UnixWrite(fdes, idbuf, idsize);
	}
	cnt += i2 - i1;
    } // for ..
    UnixClose(fdes);
    return cnt;
} // ibis::query::recordDeltaPairs

template <>
int64_t ibis::query::recordDeltaPairs(const array_t<int32_t>& val1,
				      const array_t<uint32_t>& val2,
				      const array_t<uint32_t>& ind1,
				      const array_t<uint32_t>& ind2,
				      const int32_t& delta,
				      const char* filename) const {
    if (filename == 0 || *filename == 0)
	return countDeltaPairs(val1, val2, delta);
    if (delta <= 0)
	return recordEqualPairs(val1, val2, ind1, ind2, filename);
    int fdes = UnixOpen(filename, OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	logWarning("recordDeltaPairs",
		   "failed to open file \"%s\" for writing", filename);
	return countDeltaPairs(val1, val2, delta);
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int64_t cnt = 0;
    uint32_t idbuf[2];
    const uint32_t idsize = sizeof(idbuf);
    uint32_t i1 = 0, i2 = 0;
    const uint32_t n1 = val1.size();
    for (uint32_t i = 0;
	 i < val2.find(static_cast<unsigned>(val1.back()+delta)) && i1 < n1;
	 ++ i) {
	const int lo = static_cast<int>(val2[i] - delta);
	const int hi = static_cast<int>(val2[i] + delta);
	// move i1 to catch up with lo
	while (i1 < n1 && val1[i1] < lo)
	    ++ i1;
	// move i2 to catch up with hi
	if (i1 > i2)
	    i2 = i1;
	while (i2 < n1 && val1[i2] <= hi)
	    ++ i2;
	idbuf[1] = (ind2.size() == val2.size() ? ind2[i] : i);
	if (ind1.size() == val1.size()) {
	    for (uint32_t jj = i1; jj < i2; ++ jj) {
		idbuf[0] = ind1[jj];
		UnixWrite(fdes, idbuf, idsize);
	    }
	}
	else {
	    for (idbuf[0] = i1; idbuf[0] < i2 && idbuf[0] < n1; ++ idbuf[0])
		UnixWrite(fdes, idbuf, idsize);
	}
	cnt += i2 - i1;
    } // for ..
    UnixClose(fdes);
    return cnt;
} // ibis::query::recordDeltaPairs

/// Performing an equi-join by sorting the selected values first.  This
/// version reads the values marked to be 1 in the bitvector @c mask and
/// performs the actual operation of counting the number of pairs with
/// equal values in memory.
int64_t ibis::query::sortEquiJoin(const ibis::rangeJoin& cmp,
				  const ibis::bitvector& mask) const {
    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();

    long ierr = 0;
    const ibis::column *col1 = table0->getColumn(cmp.getName1());
    const ibis::column *col2 = table0->getColumn(cmp.getName2());
    if (col1 == 0) {
	logWarning("sortEquiJoin", "can not find the named column (%s)",
		   cmp.getName1());
	return -1;
    }
    if (col2 == 0) {
	logWarning("sortEquiJoin", "can not find the named column (%s)",
		   cmp.getName2());
	return -2;
    }
    int64_t cnt = 0;

    switch (col1->type()) {
    case ibis::INT: {
	array_t<int32_t> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::UINT:
    case ibis::CATEGORY: {
	array_t<uint32_t> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countEqualPairs(val1, val2);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    default:
	logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		   cmp.getName1(), static_cast<int>(col1->type()));
    }

    if (ibis::gVerbose > 2) {
	timer.stop();
	std::ostringstream ostr;
	ostr << cnt << " hit" << (cnt>1 ? "s" : "");
	logMessage("sortEquiJoin", "equi-join(%s, %s) produced %s in "
		   "%g sec(CPU) and %g sec(elapsed)",
		   cmp.getName1(), cmp.getName2(), ostr.str().c_str(),
		   timer.CPUTime(), timer.realTime());
    }
    return cnt;
} // ibis::query::sortEquiJoin

/// Performing a range join by sorting the selected values.  The sorting is
/// performed through @c std::sort algorithm.
int64_t ibis::query::sortRangeJoin(const ibis::rangeJoin& cmp,
				   const ibis::bitvector& mask) const {
    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();

    long ierr = 0;
    int64_t cnt = 0;
    const ibis::column *col1 = table0->getColumn(cmp.getName1());
    const ibis::column *col2 = table0->getColumn(cmp.getName2());
    if (col1 == 0) {
	logWarning("sortRangeJoin", "can not find the named column (%s)",
		   cmp.getName1());
	return -1;
    }
    if (col2 == 0) {
	logWarning("sortRangeJoin", "can not find the named column (%s)",
		   cmp.getName2());
	return -2;
    }

    switch (col1->type()) {
    case ibis::INT: {
	const int32_t delta =
	    static_cast<int32_t>(fabs(cmp.getRange()->eval()));
	array_t<int32_t> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::UINT:
    case ibis::CATEGORY: {
	const uint32_t delta =
	    static_cast<uint32_t>(fabs(cmp.getRange()->eval()));
	array_t<uint32_t> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::FLOAT: {
	const float delta = static_cast<float>(fabs(cmp.getRange()->eval()));
	array_t<float> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::DOUBLE: {
	const double delta = fabs(cmp.getRange()->eval());
	array_t<double> val1;
	{
	    array_t<uint32_t> ind1;
	    ierr = col1->selectValues(mask, val1, ind1);
	}
	std::sort(val1.begin(), val1.end());
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    {
		array_t<uint32_t> ind2;
		ierr = col2->selectValues(mask, val2, ind2);
	    }
	    std::sort(val2.begin(), val2.end());
	    cnt = countDeltaPairs(val1, val2, delta);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    default:
	logWarning("sortRangeJoin", "column %s has a unsupported type %d",
		   cmp.getName1(), static_cast<int>(col1->type()));
    }

    if (ibis::gVerbose > 2) {
	timer.stop();
	std::ostringstream ostr;
	ostr << cnt << " hit" << (cnt>1 ? "s" : "");
	logMessage("sortRangeJoin", "range join(%s, %s, %g) produced %s "
		   "in %g sec(CPU) and %g sec(elapsed)",
		   cmp.getName1(), cmp.getName2(),
		   fabs(cmp.getRange()->eval()), ostr.str().c_str(),
		   timer.CPUTime(), timer.realTime());
    }
    return cnt;
} // ibis::query::sortRangeJoin

/// Perform equi-join by sorting the selected values.  This version reads
/// the values marked to be 1 in the bitvector @c mask.  It writes the
/// the pairs satisfying the join condition to a file name @c pairfile.
int64_t ibis::query::sortEquiJoin(const ibis::rangeJoin& cmp,
				  const ibis::bitvector& mask,
				  const char* pairfile) const {
    if (pairfile == 0 || *pairfile == 0)
	return sortEquiJoin(cmp, mask);

    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();

    long ierr = 0;
    const ibis::column *col1 = table0->getColumn(cmp.getName1());
    const ibis::column *col2 = table0->getColumn(cmp.getName2());
    if (col1 == 0) {
	logWarning("sortEquiJoin", "can not find the named column (%s)",
		   cmp.getName1());
	return -1;
    }
    if (col2 == 0) {
	logWarning("sortEquiJoin", "can not find the named column (%s)",
		   cmp.getName2());
	return -2;
    }
    int64_t cnt = 0;

    switch (col1->type()) {
    case ibis::INT: {
	array_t<int32_t> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
	{ // to limit the scope of tmp;
	    array_t<int32_t> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<int32_t>::stableSort(val1, ind1, tmp, itmp);
	}
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::UINT:
    case ibis::CATEGORY: {
	array_t<uint32_t> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
	{
	    array_t<uint32_t> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<uint32_t>::stableSort(val1, ind1, tmp, itmp);
	}
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::FLOAT: {
	array_t<float> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
	{
	    array_t<float> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<float>::stableSort(val1, ind1, tmp, itmp);
	}
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
	{
	    array_t<double> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<double>::stableSort(val1, ind1, tmp, itmp);
	}
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordEqualPairs(val1, val2, ind1, ind2, pairfile);
	    break;}
	default:
	    logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    default:
	logWarning("sortEquiJoin", "column %s has a unsupported type %d",
		   cmp.getName1(), static_cast<int>(col1->type()));
    }

    if (ibis::gVerbose > 2) {
	timer.stop();
	std::ostringstream ostr;
	ostr << cnt << " hit" << (cnt>1 ? "s" : "");
	logMessage("sortEquiJoin", "equi-join(%s, %s) produced %s in "
		   "%g sec(CPU) and %g sec(elapsed)",
		   cmp.getName1(), cmp.getName2(), ostr.str().c_str(),
		   timer.CPUTime(), timer.realTime());
    }
    return cnt;
} // ibis::query::sortEquiJoin

/// Performing range join by sorting the selected values.
int64_t ibis::query::sortRangeJoin(const ibis::rangeJoin& cmp,
				   const ibis::bitvector& mask,
				   const char* pairfile) const {
    if (pairfile == 0 || *pairfile == 0)
	return sortRangeJoin(cmp, mask);

    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();

    long ierr = 0;
    int64_t cnt = 0;
    const ibis::column *col1 = table0->getColumn(cmp.getName1());
    const ibis::column *col2 = table0->getColumn(cmp.getName2());
    if (col1 == 0) {
	logWarning("sortRangeJoin", "can not find the named column (%s)",
		   cmp.getName1());
	return -1;
    }
    if (col2 == 0) {
	logWarning("sortRangeJoin", "can not find the named column (%s)",
		   cmp.getName2());
	return -2;
    }

    switch (col1->type()) {
    case ibis::INT: {
	const int32_t delta =
	    static_cast<int32_t>(fabs(cmp.getRange()->eval()));
	array_t<int32_t> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
	{
	    array_t<int32_t> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<int32_t>::stableSort(val1, ind1, tmp, itmp);
	}
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::UINT:
    case ibis::CATEGORY: {
	const uint32_t delta =
	    static_cast<uint32_t>(fabs(cmp.getRange()->eval()));
	array_t<uint32_t> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
#if defined(DEBUG)
	for (uint32_t i = 0; i < ind1.size(); ++ i)
	    if (ind1[i] > table0->nRows())
		logWarning("sortRangeJoin", "before sorting: ind1[%lu] = %lu "
			   "is out of range (shoud be < %lu)",
			   static_cast<long unsigned>(i),
			   static_cast<long unsigned>(ind1[i]),
			   static_cast<long unsigned>(table0->nRows()));
#endif
	{
	    array_t<uint32_t> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<uint32_t>::stableSort(val1, ind1, tmp, itmp);
	}
#if defined(DEBUG)
	for (uint32_t i = 0; i < ind1.size(); ++ i)
	    if (ind1[i] > table0->nRows())
		logWarning("sortRangeJoin", "after sorting: ind1[%lu] = %lu "
			   "is out of range (shoud be < %lu)",
			   static_cast<long unsigned>(i),
			   static_cast<long unsigned>(ind1[i]),
			   static_cast<long unsigned>(table0->nRows()));
#endif
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::FLOAT: {
	const float delta = static_cast<float>(fabs(cmp.getRange()->eval()));
	array_t<float> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
	{
	    array_t<float> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<float>::stableSort(val1, ind1, tmp, itmp);
	}
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    case ibis::DOUBLE: {
	const double delta = fabs(cmp.getRange()->eval());
	array_t<double> val1;
	array_t<uint32_t> ind1;
	ierr = col1->selectValues(mask, val1, ind1);
	{
	    array_t<double> tmp(val1.size());
	    array_t<uint32_t> itmp(val1.size());
	    array_t<double>::stableSort(val1, ind1, tmp, itmp);
	}
	switch (col2->type()) {
	case ibis::INT: {
	    array_t<int32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<int32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<int32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<uint32_t> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<uint32_t>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::FLOAT: {
	    array_t<float> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<float> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<float>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> val2;
	    array_t<uint32_t> ind2;
	    ierr = col2->selectValues(mask, val2, ind2);
	    {
		array_t<double> tmp(val2.size());
		array_t<uint32_t> itmp(val2.size());
		array_t<double>::stableSort(val2, ind2, tmp, itmp);
	    }
	    cnt = recordDeltaPairs(val1, val2, ind1, ind2, delta, pairfile);
	    break;}
	default:
	    logWarning("sortRangeJoin",
		       "column %s has a unsupported type %d",
		       cmp.getName2(), static_cast<int>(col2->type()));
	}
	break;}
    default:
	logWarning("sortRangeJoin", "column %s has a unsupported type %d",
		   cmp.getName1(), static_cast<int>(col1->type()));
    }

    if (ibis::gVerbose > 2) {
	timer.stop();
	std::ostringstream ostr;
	ostr << cnt << " hit" << (cnt>1 ? "s" : "");
	logMessage("sortRangeJoin", "range join(%s, %s, %g) produced %s in "
		   "%g sec(CPU) and %g sec(elapsed)",
		   cmp.getName1(), cmp.getName2(),
		   fabs(cmp.getRange()->eval()), ostr.str().c_str(),
		   timer.CPUTime(), timer.realTime());
    }
    return cnt;
} // ibis::query::sortRangeJoin

/// Sort the content of the file as ibis::rid_t.  It reads the content
/// of the file one block at a time during the initial sorting of the
/// blocks.  It then merges the sorted blocks to produce a overall sorted
/// file.  Note that ibis::rid_t is simply a pair of integers.  Sinc the
/// pairs are recorded as pairs of integers too, this should work.
void ibis::query::orderPairs(const char *pfile) const {
    if (pfile == 0 || *pfile == 0)
	return;
    uint32_t npairs = ibis::util::getFileSize(pfile);
    int fdes = UnixOpen(pfile, OPEN_READWRITE);
    long ierr;
    if (fdes < 0) {
	logWarning("orderPairs", "unable to open %s for sorting", pfile);
	return;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif
#ifdef DEBUG
    if (ibis::fileManager::instance().bytesFree() > npairs) {
#endif
	npairs /= sizeof(ibis::rid_t);
	try {
	    const long unsigned nbytes = npairs * sizeof(ibis::rid_t);
	    array_t<ibis::rid_t> tmp(npairs);
	    ierr = UnixRead(fdes, tmp.begin(), nbytes);
	    if (ierr >= static_cast<long>(nbytes)) {
		std::sort(tmp.begin(), tmp.end());
		ierr = UnixSeek(fdes, 0, SEEK_SET);
		ierr = UnixWrite(fdes, tmp.begin(), nbytes);
		if (ierr != static_cast<long>(nbytes))
		    logWarning("orderPairs", "expected to write %lu bytes "
			       "to %s, but only wrote %ld",
			       nbytes, pfile, ierr);
		UnixClose(fdes);
		return;
	    }
	    else {
		logMessage("orderPairs", "failed to read all %lu bytes "
			   "from %s in one shot (ierr=%ld), "
			   "will use out-of-core sorting",
			   nbytes, pfile, ierr);
	    }
	}
	catch (...) {
	    logMessage("orderPairs", "received an exception (like because "
		       "there is not enough memory to read the whole "
		       "content of %s), will use out-of-core sorting",
		       pfile);
	}
#ifdef DEBUG
    }
#endif
#ifdef DEBUG
    const uint32_t mblock = PREFERRED_BLOCK_SIZE / (2*sizeof(uint32_t));
    array_t<ibis::rid_t> buf1(mblock), buf2(mblock);
    // the initial sorting of the blocks.
    bool more = true;
    while (more) {
	ierr = UnixRead(fdes, buf1.begin(), mblock*sizeof(ibis::rid_t));
	if (ierr > 0) {
	    long bytes = ierr;
	    ierr /= sizeof(ibis::rid_t);
	    npairs += ierr;
	    buf1.resize(ierr);
	    buf1.stableSort(buf2);  // sort the block
	    // write back the sorted values
	    ierr = UnixSeek(fdes, -bytes, SEEK_CUR);
	    if (ierr == -1) {
		logWarning("orderPairs", "UnixSeek on %s encountered an "
			   "error, can not proceed anymore",
			   pfile);
		UnixClose(fdes);
		return;
	    }
	    ierr = UnixWrite(fdes, buf1.begin(), bytes);
	    if (ierr != bytes) {
		logWarning("orderPairs", "expected to write %ld bytes, "
			   "but actually wrote %d", bytes, ierr);
	    }
	}
	else {
	    more = false;
	}
    }
    UnixClose(fdes);
    if (ibis::gVerbose > 6)
	logMessage("orderPairs", "complete sorting file %s in blocks of "
		   "size %lu (total %lu)", pfile,
		   static_cast<long unsigned>(mblock),
		   static_cast<long unsigned>(npairs));

    // merge the sorted blocks
    const uint32_t totbytes = npairs*sizeof(ibis::rid_t);
    const uint32_t bytes = mblock * sizeof(ibis::rid_t);
    uint32_t stride = bytes;
    array_t<ibis::rid_t> buf3(mblock);
    std::string tmpfile(pfile);
    tmpfile += "-tmp";
    const char *name1 = pfile;
    const char *name2 = tmpfile.c_str();
    while (stride < totbytes) {
	if (ibis::gVerbose > 6)
	    logMessage("orderPairs",
		       "merging block from %lu bytes apart in %s",
		       static_cast<long unsigned>(stride), pfile);
	ibis::roster::mergeBlock2<ibis::rid_t>(name1, name2, stride,
					       buf1, buf2, buf3);
	const char *stmp = name1;
	name1 = name2;
	name2 = stmp;
	stride += stride;
    }
    remove(name2); // remove the temp file
    if (name1 != pfile) {
	rename(name1, name2);
    }
#else
    logWarning("orderPairs", "out-of-core version does not work yet");
#endif
} // ibis::query::orderPairs

int64_t ibis::query::mergePairs(const char *pfile) const {
    if (pfile == 0 || *pfile == 0)
	return 0;

    uint32_t buf1[2], buf2[2];
    const uint32_t idsize = sizeof(buf1);
    int64_t cnt = ibis::util::getFileSize(pfile);
    cnt /= idsize;
    if (cnt <= 0)
	return cnt;

    std::string oldfile(myDir);
    std::string outfile(myDir);
    oldfile += "oldpairs";
    outfile += "pairs";
    const uint32_t incnt = cnt;
    const uint32_t oldcnt = ibis::util::getFileSize(outfile.c_str()) / idsize;
    if (oldcnt == 0) {
	// the output file does not exist, simply copy the input file to
	// the output file
	ibis::util::copy(outfile.c_str(), pfile);
	return cnt;
    }

    long ierr = rename(outfile.c_str(), oldfile.c_str());
    if (ierr != 0) {
	logWarning("mergePairs", "unable to rename \"%s\" to \"%s\"",
		   outfile.c_str(), oldfile.c_str());
	return -1;
    }
    cnt = 0;
    int indes = UnixOpen(pfile, OPEN_READONLY);
    if (indes < 0) {
	logWarning("mergePairs", "unable to open %s for reading", pfile);
	return -2;
    }

    int outdes = UnixOpen(outfile.c_str(), OPEN_WRITEONLY, OPEN_FILEMODE);
    if (outdes < 0) {
	logWarning("mergePairs", "unable to open %s for writing",
		   outfile.c_str());
	UnixClose(indes);
	return -3;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(outdes, _O_BINARY);
#endif

    int olddes = UnixOpen(oldfile.c_str(), OPEN_READONLY);
    if (olddes < 0) {
	logWarning("mergePairs", "unable to open %s for reading",
		   oldfile.c_str());
	UnixClose(outdes);
	UnixClose(indes);
	return -4;
    }

    ierr = UnixRead(indes, buf1, idsize);
    ierr += UnixRead(olddes, buf2, idsize);
    while (ierr >= static_cast<int>(idsize)) {
	while (ierr >= static_cast<int>(idsize) &&
	       (buf1[0] < buf2[0] ||
		(buf1[0] == buf2[0] && buf1[1] < buf2[1]))) {
#ifdef DEBUG
	    uint32_t tmp[2];
	    ierr = UnixRead(indes, tmp, idsize);
	    if (tmp[0] < buf1[0] ||
		(tmp[0] == buf1[0] && tmp[1] < buf1[1]))
		logWarning("mergePairs", "%s not sorted -- pairs (%lu, %lu) "
			   "and (%lu, %lu) out of order",
			   pfile, static_cast<long unsigned>(buf1[0]),
			   static_cast<long unsigned>(buf1[1]),
			   static_cast<long unsigned>(tmp[0]),
			   static_cast<long unsigned>(tmp[1]));
	    buf1[0] = tmp[0];
	    buf1[1] = tmp[1];
#else
	    ierr = UnixRead(indes, buf1, idsize);
#endif
	}
	while (ierr >= static_cast<int>(idsize) &&
	       (buf1[0] > buf2[0] ||
		(buf1[0] == buf2[0] && buf1[1] > buf2[1]))) {
#ifdef DEBUG
	    uint32_t tmp[2];
	    ierr = UnixRead(olddes, tmp, idsize);
	    if (tmp[0] < buf2[0] ||
		(tmp[0] == buf2[0] && tmp[1] < buf2[1]))
		logWarning("mergePairs", "%s not sorted -- pairs (%lu, %lu) "
			   "and (%lu, %lu) out of order", oldfile.c_str(),
			   static_cast<long unsigned>(buf2[0]),
			   static_cast<long unsigned>(buf2[1]),
			   static_cast<long unsigned>(tmp[0]),
			   static_cast<long unsigned>(tmp[1]));
	    buf2[0] = tmp[0];
	    buf2[1] = tmp[1];
#else
	    ierr = UnixRead(olddes, buf2, idsize);
#endif
	}
	if (ierr >= static_cast<int>(idsize) &&
	    buf1[0] == buf2[0] && buf1[1] == buf2[1]) {
	    ierr = UnixWrite(outdes, buf1, idsize);
	    if (ierr >= static_cast<int>(idsize)) {
		++ cnt;
	    }
	    else {
		logWarning("mergePairs", "failed to write %ld-th pair to %s",
			   static_cast<long>(cnt), outfile.c_str());
		ierr = UnixSeek(outdes, cnt*idsize, SEEK_SET);
	    }

	    ierr = UnixRead(indes, buf1, idsize);
	    if (ierr >= static_cast<int>(idsize))
		ierr = UnixRead(olddes, buf2, idsize);
	}
    } // while (ierr >= idsize)

    UnixClose(olddes);
    UnixClose(outdes);
    UnixClose(indes);
    remove(oldfile.c_str());
    if (ibis::gVerbose > 4)
	logMessage("mergePairs", "comparing %lu pairs from \"%s\" with "
		   "%lu pairs in \"pairs\" produced %lu common ones",
		   static_cast<long unsigned>(incnt), pfile,
		   static_cast<long unsigned>(oldcnt),
		   static_cast<long unsigned>(cnt));
    return cnt;
} // ibis::query::mergePairs

////////////////////////////////////////////////////////////////////////
/// Parse the incoming string into a list of names plus functions.
void ibis::selected::select(const char *str, bool sort) {
    clear(); // always clear the existing content

    uint32_t terms = 0;
    const char *s = str;
    while (s != 0 && *s != 0) {
	// remove leading space and delimiters
	s += strspn(s, ibis::util::delimiters);
	if (*s == 0) break;

	functions.push_back(ibis::selected::NIL);
	std::string tmp;
	while (isprint(*s) && *s != '(' &&
	       strchr(ibis::util::delimiters, *s) == 0) {
	    tmp += *s; // append the character;
	    ++ s;
	}
	if (*s == '(') { // tmp contains a function name
	    if (tmp.size() != 3) {
		if (ibis::gVerbose > 1)
		    ibis::util::logMessage("ibis::selected",
					   "select(%s) skipping function name "
					   "(%s) that is not three-byte long",
					   str, tmp.c_str());
	    }
	    else if ((tmp[0] == 'm' || tmp[0] == 'M')) {
		if (tmp[1] == 'i' || tmp[1] == 'I' &&
		    (tmp[2] == 'n' || tmp[2] == 'N')) {
		    functions[terms] = ibis::selected::MIN;
		}
		else if (tmp[1] == 'a' || tmp[1] == 'A' &&
			 (tmp[2] == 'x' || tmp[2] == 'X')) {
		    functions[terms] = ibis::selected::MAX;
		}
		else if (ibis::gVerbose > 1) {
		    ibis::util::logMessage("ibis::selected",
					   "select(%s) skipping unknown "
					   "function name (%s)",
					   str, tmp.c_str());
		}
	    }
	    else if ((tmp[0] == 'a' || tmp[0] == 'A') &&
		     (tmp[1] == 'v' || tmp[1] == 'V') &&
		     (tmp[2] == 'g' || tmp[2] == 'G')) {
		functions[terms] = ibis::selected::AVG;
	    }
	    else if ((tmp[0] == 's' || tmp[0] == 'S') &&
		     (tmp[1] == 'u' || tmp[1] == 'U') &&
		     (tmp[2] == 'm' || tmp[2] == 'M')) {
		functions[terms] = ibis::selected::SUM;
	    }
	    else if (ibis::gVerbose > 1) {
		ibis::util::logMessage("ibis::selected",
				       "select(%s) skipping unknown "
				       "function name (%s)",
				       str, tmp.c_str());
	    }

	    tmp.erase();
	    ++ s;
	}

	if (*s) {
	    while (isprint(*s) && *s != ')' &&
		   strchr(ibis::util::delimiters, *s) == 0) {
		tmp += *s;
		++ s;
	    }
	    s += (*s == ')');
	}

	if (tmp.empty() || tmp[0] == '*') { // not column name
	    functions.resize(terms); // remove the function name
	}
	else { // record function name
	    nplain += (functions[terms] == ibis::selected::NIL);
	    names.push_back(tmp);
	    ++ terms;
	}
    }

    if (nplain < names.size() && sort) {
	// move funtions the end of the list
	std::vector<std::string> nms(terms);
	std::vector<ibis::selected::FUNCTION>
	    func(terms, ibis::selected::NIL);
	uint32_t iplain = 0;
	uint32_t ifunc = nplain;
	for (uint32_t i = 0; i < terms && iplain < nplain; ++ i) {
	    if (functions[i] == ibis::selected::NIL) {
		nms[iplain].swap(names[i]);
		++ iplain;
	    }
	    else {
		nms[ifunc].swap(names[i]);
		func[ifunc] = functions[i];
		++ ifunc;
	    }
	}
	for (uint32_t i = 0; i < ifunc; ++ i) {
	    functions[i] = func[i];
	    names[i].swap(nms[i]);
	}
    }

    toString(mystr_); // fill the single string form of the select clause
    if (ibis::gVerbose > 1) {
	if (mystr_.empty())
	    ibis::util::logMessage("ibis::selected::select",
				   "empty select clause");
	else
	    ibis::util::logMessage("ibis::selected::select",
				   "SELECT %s", mystr_.c_str());
    }
} // ibis::selected::select

void ibis::selected::select(const std::vector<const char*>& nl, bool sort) {
    if (nl.empty()) return;

    functions.clear();
    names.clear();
    nplain = 0;
    for (size_t i = 0; i < nl.size(); ++ i) {
	const char* tmp = strchr(nl[i], '(');
	if (tmp > nl[i]) { // try to determine the function
	    for (++ tmp; *tmp != 0 && isspace(*tmp) != 0; ++ tmp);
	    if (*tmp == 0) continue;
	    names.push_back(tmp);
	    size_t pos = names.back().find(')');
	    if (pos < names.back().size())
		names.back().erase(pos);
	    if (names.back().empty()) { // can not use an empty string
		names.resize(names.size()-1);
		continue;
	    }

	    if ((nl[i][0] == 'a' || nl[i][0] == 'A') &&
		(nl[i][1] == 'v' || nl[i][1] == 'V') &&
		(nl[i][2] == 'g' || nl[i][2] == 'G')) {
		functions.push_back(ibis::selected::AVG);
	    }
	    else if ((nl[i][0] == 's' || nl[i][0] == 'S') &&
		     (nl[i][1] == 'u' || nl[i][1] == 'U') &&
		     (nl[i][2] == 'm' || nl[i][2] == 'M')) {
		functions.push_back(ibis::selected::SUM);
	    }
	    else if ((nl[i][0] == 'm' || nl[i][0] == 'M') &&
		     (nl[i][1] == 'i' || nl[i][1] == 'I') &&
		     (nl[i][2] == 'n' || nl[i][2] == 'N')) {
		functions.push_back(ibis::selected::MIN);
	    }
	    else if ((nl[i][0] == 'm' || nl[i][0] == 'M') &&
		     (nl[i][1] == 'a' || nl[i][1] == 'A') &&
		     (nl[i][2] == 'x' || nl[i][2] == 'X')) {
		functions.push_back(ibis::selected::MAX);
	    }
	    else {
		ibis::util::logMessage("ibis::selected::select",
				       "string \"%s\" contains an unknown "
				       "function, skipping it", nl[i]);
		names.resize(names.size()-1);
		continue;
	    }
	}
	else {
	    ++ nplain;
	    names.push_back(nl[i]);
	    functions.push_back(ibis::selected::NIL);
	}
    }

    if (nplain < names.size() && sort) {
	uint32_t jp = 0, jf = nplain;
	std::vector<std::string> nm2(names.size());
	std::vector<ibis::selected::FUNCTION> fn2(names.size());
	nm2.swap(names);
	fn2.swap(functions);
	for (uint32_t i = 0; i < nm2.size(); ++ i) {
	    if (fn2[i] == NIL) {
		functions[jp] = fn2[i];
		names[jp] = nm2[i];
		++ jp;
	    }
	    else {
		functions[jf] = fn2[i];
		names[jf] = nm2[i];
		++ jf;
	    }
	}
    }

    toString(mystr_); // fill the single string form of the select clause
    if (ibis::gVerbose > 1) {
	if (mystr_.empty())
	    ibis::util::logMessage("ibis::selected::select",
				   "empty select clause");
	else
	    ibis::util::logMessage("ibis::selected::select",
				   "SELECT %s", mystr_.c_str());
    }
} // ibis::selected::select

/// Use a simple linear search to locate the desired column name.
size_t ibis::selected::find(const char *key) const {
    size_t i;
    for (i = 0; i < names.size(); ++ i)
	if (stricmp(names[i].c_str(), key) == 0)
	    break;
    return i;
} // ibis::selected::find

std::string ibis::selected::getTerm(size_t i) const {
    std::string res;
    if (i < names.size()) {
	if (functions[i] == AVG) {
	    res = "AVG(";
	    res += names[i];
	    res += ')';
	}
	else if (functions[i] == MAX) {
	    res = "MAX(";
	    res += names[i];
	    res += ')';
	}
	else if (functions[i] == MIN) {
	    res = "MIN(";
	    res += names[i];
	    res += ')';
	}
	else if (functions[i] == SUM) {
	    res = "SUM(";
	    res += names[i];
	    res += ')';
	}
	else {
	    res = names[i];
	}
    }
    return res;
} // ibis::selected::getTerm

std::string ibis::selected::uniqueNames() const {
    if (names.empty()) {
	std::string tmp;
	return tmp;
    }
    else if (names.size() == 1) {
	return names[0];
    }
    else {
	std::set<const char*, ibis::lessi> unames;
	unames.insert(names[0].c_str());
	for (size_t i = 1; i < names.size(); ++ i)
	    unames.insert(names[i].c_str());

	std::set<const char*, ibis::lessi>::const_iterator it = unames.begin();
	std::ostringstream oss;
	oss << *it;
	for (++ it; it != unames.end(); ++ it)
	    oss << ", " << *it;
	return oss.str();
    }
} // ibis::selected::uniqueNames

void ibis::selected::toString(std::string& str) const {
    str.clear();
    if (! names.empty()) {
	std::ostringstream ostr;
	print(0, ostr);
	for (uint32_t i = 1; i < names.size(); ++ i) {
	    ostr << ", ";
	    print(i, ostr);
	}
	str = ostr.str();
    }
} // ibis::selected::toString

void ibis::selected::remove(const std::vector<size_t>& ents) {
    const size_t size0 = names.size();
    size_t terms = 0;
    if (ents.size() >= size0) {
	clear();
	return;
    }
    else {
	// erase the names to be removed;
	for (size_t i = 0; i < ents.size(); ++ i)
	    if (ents[i] < size0)
		names[ents[i]].erase();
	// pack the two arrays names and functions, update variable nplain
	nplain = 0;
	for (size_t i = 0; i < size0; ++ i) {
	    if (! names[i].empty()) {
		if (terms < i) // swap the name
		    names[i].swap(names[terms]);
		nplain += (functions[i] == ibis::selected::NIL);
		functions[terms] = functions[i];
		++ terms;
	    }
	}
	names.resize(terms);
	functions.resize(terms);
    }

    if (ibis::gVerbose > 4)
	ibis::util::logMessage("ibis::selected::remove",
			       "reduce the number of terms from %lu to %lu",
			       static_cast<long unsigned>(size0),
			       static_cast<long unsigned>(terms));
} // ibis::selected::remove

void ibis::selected::print(size_t i, std::ostream& out) const {
    switch (functions[i]) {
    default:
    case NIL:
	out << names[i]; break;
    case AVG:
	out << "AVG(" << names[i] << ")"; break;
    case MAX:
	out << "MAX(" << names[i] << ")"; break;
    case MIN:
	out << "MIN(" << names[i] << ")"; break;
    case SUM:
	out << "SUM(" << names[i] << ")"; break;
    }
} // ibis::selected::print