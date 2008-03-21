// $Id$
// Author: John Wu <John.Wu@ACM.org> Lawrence Berkeley National Laboratory
// Copyright 2007-2008 the Regents of the University of California
/** @file ardea.cpp

This is a simple test program for functions defined in ibis::tablex.

The user may specify a set of records to be read by using a combination of
-m option (for meta data, i.e., column names and types) and -t or -r
options.  Option -t is used to specify the name of a CSV file and option -r
is used to specify one line of CSV data on command line.

The caller may further specify a number of queries to be run on the data
after they are written to disk.

If the user did not specify any new data.  It will write a built-in set of
data (91 rows and 8 columns) and then run 10 queries with known numbers of
hits.

If the directory specified in -d option (default to "tmp") contains data,
the new records will be appended.  When the names match, the records are
assumed to the same type (not checked).  When the names do not match, the
rows with missing values are padded with NULL values.  See @c
ibis::tablex::appendRow for more information about NULL values.

@note Ardea ibis, the Latin name for <A HREF="http://www.birds.cornell.edu/AllAboutBirds/BirdGuide/Cattle_Egret.html">Cattle Egret</A>.
*/
#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)	// some identifier longer than 256 characters
#endif
#include "table.h"	// ibis::table
#include "resource.h"	// ibis::gParameters
#include <set>		// std::set

// local data types
typedef std::set< const char*, ibis::lessi > qList;
static std::vector<const char*> inputrows;
static std::vector<const char*> csvfiles;
static std::string metadata;

// printout the usage string
static void usage(const char* name) {
    std::cout << "usage:\n" << name << " [-c conf-file] "
	"[-d directory-to-write-data] [-n name-of-dataset] "
	"[-r a-row-in-ASCII-form] [-t text-file-to-read] [-b break/delimiters-in-text-file]"
	"[-m name:type[,name:type,...]] [-s select-clause]"
	"[-w where-clause] [-v[=| ]verbose_level]\n\n"
	"Note:\n\tColumn name must start with an alphabet and can only contain alphanumeric values\n"
	"\tThis program only recognize the following column types:\n"
	"\tbyte, short, int, long, float, double, key, and text\n"
	"\tIt only checks the first character of the types.\n"
	"\tFor example, one can load the data in tests/test0.csv either one of the following command lines:\n"
	"\tardea -d somwhere1 -m a:i,b:i,c:i -t tests/test0.csv\n"
	"\tardea -d somwhere2 -m a:i,b:f,c:d -t tests/test0.csv\n"
	      << std::endl;
} // usage

// Adds a table defined in the named directory.
static void addTables(ibis::tableList& tlist, const char* dir) {
    ibis::table *tbl = ibis::table::create(dir);
    if (tbl == 0) return;
    if (tbl->nRows() != 0 && tbl->nColumns() != 0)
	tlist.add(tbl);
    delete tbl;
} // addTables

// function to parse the command line arguments
static void parse_args(int argc, char** argv, qList& qcnd, const char*& sel,
		       const char*& outdir, const char*& dsname,
		       const char*& del) {
    sel = 0;
    for (int i=1; i<argc; ++i) {
	if (*argv[i] == '-') { // normal arguments starting with -
	    switch (argv[i][1]) {
	    default:
	    case 'h':
	    case 'H':
		usage(*argv);
		exit(0);
	    case 'b':
	    case 'B': // break/delimiters
		if (i+1 < argc) {
		    ++ i;
		    del = argv[i];
		}
		break;
	    case 'c':
	    case 'C':
		if (i+1 < argc) {
		    ++ i;
		    ibis::gParameters().read(argv[i]);
		}
		break;
	    case 'd':
	    case 'D':
		if (i+1 < argc) {
		    ++ i;
		    outdir = argv[i];
		}
		break;
	    case 'm':
	    case 'M':
		if (i+1 < argc) {
		    ++ i;
		    if (! metadata.empty())
			metadata += ", ";
		    metadata += argv[i];
		}
		break;
	    case 'n':
	    case 'N':
		if (i+1 < argc) {
		    ++ i;
		    dsname = argv[i];
		}
		break;
	    case 'r':
	    case 'R':
		if (i+1 < argc) {
		    ++ i;
		    inputrows.push_back(argv[i]);
		}
		break;
	    case 't':
	    case 'T':
		if (i+1 < argc) {
		    ++ i;
		    csvfiles.push_back(argv[i]);
		}
		break;
	    case 'q':
	    case 'Q':
	    case 'w':
	    case 'W':
		if (i+1 < argc) {
		    ++ i;
		    qcnd.insert(argv[i]);
		}
		break;
	    case 's':
	    case 'S':
		if (i+1 < argc) {
		    ++ i;
		    sel = argv[i];
		}
		break;
	    case 'v':
	    case 'V': {
		char *ptr = strchr(argv[i], '=');
		if (ptr == 0) {
		    if (i+1 < argc) {
			if (isdigit(*argv[i+1])) {
			    ibis::gVerbose += atoi(argv[i+1]);
			    i = i + 1;
			}
			else {
			    ++ ibis::gVerbose;
			}
		    }
		    else {
			++ ibis::gVerbose;
		    }
		}
		else {
		    ibis::gVerbose += atoi(++ptr);
		}
		break;}
	    } // switch (argv[i][1])
	} // normal arguments
	else { // assume to be a set of query conditioins
	    qcnd.insert(argv[i]);
	}
    } // for (inti=1; ...)

#if defined(DEBUG) || defined(_DEBUG)
#if DEBUG + 0 > 10 || _DEBUG + 0 > 10
    ibis::gVerbose = INT_MAX;
#elif DEBUG + 0 > 0
    ibis::gVerbose += 7 * DEBUG;
#elif _DEBUG + 0 > 0
    ibis::gVerbose += 5 * _DEBUG;
#else
    ibis::gVerbose += 3;
#endif
#endif
    std::cout << argv[0] << ": verbose level " << ibis::gVerbose << "\n";
    if (inputrows.size() > 0 || csvfiles.size() > 0) {
	if (!metadata.empty()) {
	    std::cout << "Will parse ";
	    if (csvfiles.size() > 0)
		std::cout << csvfiles.size() << " CSV file"
			  << (csvfiles.size() > 1 ? "s" : "");
	    if (csvfiles.size() > 0 && inputrows.size() > 0)
		std::cout <<  " and ";
	    if (inputrows.size() > 0)
		std::cout << inputrows.size() << " row"
			  << (inputrows.size() > 1 ? "s" : "");
	    std::cout << " with the following column names and types\n\t"
		      << metadata << "\n";
	}
	else {
	    std::clog << *argv << " can not parse the specified data without "
		      << "metadata, use option -m name:type[,name:type] "
		      << "to specify the required metadata\n";
	}
    }
    else if (! metadata.empty()) {
	std::clog << *argv << " metadata specified with -r or -t options\n";
    }
    if (qcnd.size() > 0) {
	std::cout << "User-supplied queries: ";
	for (qList::const_iterator it = qcnd.begin(); it != qcnd.end(); ++it)
	    std::cout << "  " << *it << "\n";
    }
    std::cout << std::endl;
} // parse_args

static void clearBuffers(const ibis::table::typeList& tps,
			 std::vector<void*>& buffers) {
    const size_t nc = (tps.size() <= buffers.size() ?
		       tps.size() : buffers.size());
    for (size_t j = 0; j < nc; ++ j) {
	switch (tps[j]) {
	case ibis::BYTE: {
	    signed char* tmp = static_cast<signed char*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::UBYTE: {
	    unsigned char* tmp = static_cast<unsigned char*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::SHORT: {
	    int16_t* tmp = static_cast<int16_t*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::USHORT: {
	    uint16_t* tmp = static_cast<uint16_t*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::INT: {
	    int32_t* tmp = static_cast<int32_t*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::UINT: {
	    uint32_t* tmp = static_cast<uint32_t*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::LONG: {
	    int64_t* tmp = static_cast<int64_t*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::ULONG: {
	    uint64_t* tmp = static_cast<uint64_t*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::FLOAT: {
	    float* tmp = static_cast<float*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::DOUBLE: {
	    double* tmp = static_cast<double*>(buffers[j]);
	    delete [] tmp;
	    break;}
	case ibis::TEXT:
	case ibis::CATEGORY: {
		std::vector<std::string>* tmp =
		    static_cast<std::vector<std::string>*>(buffers[j]);
		delete tmp;
		break;}
	default: {
	    break;}
	}
    }
} // clearBuffers

static void dumpIth(size_t i, ibis::TYPE_T t, void* buf) {
    switch (t) {
    case ibis::BYTE: {
	const signed char* tmp = static_cast<const signed char*>(buf);
	std::cout << (int)tmp[i];
	break;}
    case ibis::UBYTE: {
	const unsigned char* tmp = static_cast<const unsigned char*>(buf);
	std::cout << (unsigned)tmp[i];
	break;}
    case ibis::SHORT: {
	const int16_t* tmp = static_cast<const int16_t*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::USHORT: {
	const uint16_t* tmp = static_cast<const uint16_t*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::INT: {
	const int32_t* tmp = static_cast<const int32_t*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::UINT: {
	const uint32_t* tmp = static_cast<const uint32_t*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::LONG: {
	const int64_t* tmp = static_cast<const int64_t*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::ULONG: {
	const uint64_t* tmp = static_cast<const uint64_t*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::FLOAT: {
	const float* tmp = static_cast<const float*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::DOUBLE: {
	const double* tmp = static_cast<const double*>(buf);
	std::cout << tmp[i];
	break;}
    case ibis::TEXT:
    case ibis::CATEGORY: {
	const std::vector<std::string>* tmp =
	    static_cast<const std::vector<std::string>*>(buf);
	std::cout << '"' << (*tmp)[i] << '"';
	break;}
    default: {
	break;}
    }
} // dumpIth

// Print the first few rows of a table.  This is meant as an example that
// attempts to read all records into memory.  It is likely faster than
// funtion printValues, but it may be more likely to run out of memory.
static int printValues1(const ibis::table& tbl) {
    if (ibis::gVerbose < 0) return 0;

    const size_t nr = static_cast<size_t>(tbl.nRows());
    if (nr != tbl.nRows()) {
	std::cout << "printValues is unlikely to be able to do it job "
	    "because the number of rows (" << tbl.nRows()
		  << ") is too large for it read all records into memory"
		  << std::endl;
	return -1;
    }

    ibis::table::stringList nms = tbl.columnNames();
    ibis::table::typeList tps = tbl.columnTypes();
    std::vector<void*> buffers(nms.size(), 0);
    for (size_t i = 0; i < nms.size(); ++ i) {
	switch (tps[i]) {
	case ibis::BYTE: {
	    char* buf = new char[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsBytes(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::UBYTE: {
	    unsigned char* buf = new unsigned char[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsUBytes(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::SHORT: {
	    int16_t* buf = new int16_t[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsShorts(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::USHORT: {
	    uint16_t* buf = new uint16_t[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsUShorts(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::INT: {
	    int32_t* buf = new int32_t[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsInts(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::UINT: {
	    uint32_t* buf = new uint32_t[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsUInts(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::LONG: {
	    int64_t* buf = new int64_t[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsLongs(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::ULONG: {
	    uint64_t* buf = new uint64_t[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsULongs(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::FLOAT: {
	    float* buf = new float[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsFloats(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::DOUBLE: {
	    double* buf = new double[nr];
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsDoubles(nms[i], buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	case ibis::TEXT:
	case ibis::CATEGORY: {
	    std::vector<std::string>* buf = new std::vector<std::string>();
	    if (buf == 0) { // run out of memory
		clearBuffers(tps, buffers);
		return -1;
	    }
	    int64_t ierr = tbl.getColumnAsStrings(nms[i], *buf);
	    if (ierr < 0 || ((size_t) ierr) < nr) {
		clearBuffers(tps, buffers);
		return -2;
	    }
	    buffers[i] = buf;
	    break;}
	default: break;
	}
    }
    if (nms.size() != tbl.nColumns() || nms.size() == 0) return -3;

    size_t nprt = 10;
    if (ibis::gVerbose > 30) {
	nprt = nr;
    }
    else if ((1U << ibis::gVerbose) > nprt) {
	nprt = (1U << ibis::gVerbose);
    }
    if (nprt > nr)
	nprt = nr;
    for (size_t i = 0; i < nprt; ++ i) {
	dumpIth(i, tps[0], buffers[0]);
	for (size_t j = 1; j < nms.size(); ++ j) {
	    std::cout << ", ";
	    dumpIth(i, tps[j], buffers[j]);
	}
	std::cout << "\n";
    }
    clearBuffers(tps, buffers);

    if (nprt < nr)
	std::cout << "-- " << (nr - nprt) << " skipped...\n";
    //std::cout << std::endl;
    return 0;
} // printValues1

// This version uses ibis::cursor to print the first few rows.  It is
// likely to be slower than printValues1, but is likely to use less memory
// and less prone to failure.
static int printValues2(const ibis::table& tbl) {
    ibis::table::cursor *cur = tbl.createCursor();
    if (cur == 0) return -1;
    uint64_t nr = tbl.nRows();
    size_t nprt = 10;
    if (ibis::gVerbose > 30) {
	nprt = static_cast<size_t>(nr);
    }
    else if ((1U << ibis::gVerbose) > nprt) {
	nprt = (1U << ibis::gVerbose);
    }
    if (nprt > nr)
	nprt = static_cast<size_t>(nr);
    int ierr;
    for (size_t i = 0; i < nprt; ++ i) {
	ierr = cur->fetch(); // make the next row ready
	if (ierr == 0) {
	    cur->dump(std::cout, ", ");
	}
	else {
	    std::cout << "printValues2 failed to fetch row " << i << std::endl;
	    ierr = -2;
	    nprt = i;
	    break;
	}
    }
    delete cur; // clean up the cursor

    if (nprt < nr)
	std::cout << "-- " << (nr - nprt) << " skipped...\n";
    //std::cout << std::endl;
    return ierr;
} // printValues2

static void printValues(const ibis::table& tbl) {
    if (tbl.nColumns() == 0 || tbl.nRows() == 0) return;
    int ierr = printValues1(tbl); // try to faster version first
    if (ierr < 0) { // try to the slower version
	ierr = printValues2(tbl);
	if (ierr < 0)
	    std::cout << "printValues failed with error code " << ierr
		      << std::endl;
    }
} // printValues

// evaluate a single query, print out the number of hits
static void doQuery(const ibis::table& tbl, const char* wstr,
		    const char* sstr) {
    if (wstr == 0 || *wstr == 0) return;

    uint64_t n0, n1;
    if (ibis::gVerbose > 0) {
	tbl.estimate(wstr, n0, n1);
	std::cout << "doQuery(" << wstr
		  << ") -- the estimated number of hits on "
		  << tbl.name() << " is ";
	if (n1 > n0)
	    std::cout << "between " << n0 << " and " << n1 << "\n";
	else
	    std::cout << n1 << "\n";
	if (n1 == 0U) return;
    }

    // function select returns a table containing the selected values
    ibis::table *selected = tbl.select(sstr, wstr);
    if (selected == 0) {
	std::cout << "doQuery(" << wstr << ") failed to produce any result"
		  << std::endl;
	return;
    }

    n0 = selected->nRows();
    n1 = tbl.nRows();
    std::cout << "doQuery(" << wstr << ") evaluated on " << tbl.name()
	      << " produced " << n0 << " hit" << (n0>1 ? "s" : "")
	      << " out of " << n1 << " record" << (n1>1 ? "s" : "")
	      << "\n";
    if (ibis::gVerbose > 0) {
	std::cout << "-- begin printing the table of results --\n";
	selected->describe(std::cout); // ask the table to describe itself

	if (ibis::gVerbose > 0 && n0 > 0 && selected->nColumns() > 0)
	    printValues(*selected);
	std::cout << "--  end  printing the table of results --\n";
    }
    std::cout << std::endl;
    delete selected;
} // doQuery

static void parseNamesTypes(ibis::tablex& tbl) {
    const char* str = metadata.c_str();
    std::string nm;
    char type = 'i'; // initial default type is int
    while (*str != 0) {
	// skip leading space
	while (*str != 0 && isspace(*str)) ++ str;
	if (*str == 0) return;

	// extract a name
	nm.clear();
	while (*str != 0 && isalpha(*str) == 0) ++ str;
	while (*str != 0 && isalnum(*str) != 0) {
	    nm += *str;
	    ++ str;
	}
	if (nm.empty()) return; // did not get a name

	// skip space
	while (*str != 0 && isspace(*str)) ++ str;
	if (*str == ':') { // try to find the type
	    ++ str;
	    bool nextname = (*str != 0 && isspace(*str));
	    while (*str != 0 && isspace(*str)) ++ str;
	    if (strchr("bsilfdktBSILFDKT", *str) == 0) {
		std::clog << '\'' << *str << "\' is not one of bsilfdkt, "
			  << "assume column \"" << nm << "\" is of type "
			  << type << std::endl;
		if (! nextname) // skip the rest of the word
		    while (*str != 0 && isalnum(*str) != 0) ++ str;
	    }
	    else {
		type = *str;
		while (*str != 0 && isalnum(*str) != 0) ++ str;
	    }
	}
	if (*str != 0)
	    str += strspn(str, ",; \t");

	switch (type) {
	case 'b':
	case 'B':
	    tbl.addColumn(nm.c_str(), ibis::BYTE);
	    break;
	case 's':
	case 'S':
	    tbl.addColumn(nm.c_str(), ibis::SHORT);
	    break;
	case 'i':
	case 'I':
	default:
	    tbl.addColumn(nm.c_str(), ibis::INT);
	    break;
	case 'l':
	case 'L':
	    tbl.addColumn(nm.c_str(), ibis::LONG);
	    break;
	case 'f':
	case 'F':
	    tbl.addColumn(nm.c_str(), ibis::FLOAT);
	    break;
	case 'd':
	case 'D':
	    tbl.addColumn(nm.c_str(), ibis::DOUBLE);
	    break;
	case 'k':
	case 'K':
	    tbl.addColumn(nm.c_str(), ibis::CATEGORY);
	    break;
	case 't':
	case 'T':
	    tbl.addColumn(nm.c_str(), ibis::TEXT);
	    break;
	}
    }
} // parseNamesTypes

int main(int argc, char** argv) {
    const char* outdir = "tmp";
    const char* sel;
    const char* dsn = 0;
    const char* del = 0; // delimiters
    int ierr;
    qList qcnd;

    parse_args(argc, argv, qcnd, sel, outdir, dsn, del);
    bool usersupplied = (! metadata.empty() &&
			 (inputrows.size() > 0 || csvfiles.size() > 0));
    // create a new table that does not support querying
    ibis::tablex* ta = ibis::tablex::create();
    if (usersupplied) { // use user-supplied data
	parseNamesTypes(*ta);
	for (size_t i = 0; i < csvfiles.size(); ++ i) {
	    ierr = ta->readCSV(csvfiles[i], del);
	    if (ierr < 0)
		std::clog << *argv << " failed to parse file \""
			  << csvfiles[i] << "\", readCSV returned "
			  << ierr << std::endl;
	}
	for (size_t i = 0; i < inputrows.size(); ++ i) {
	    ierr = ta->appendRow(inputrows[i], del);
	    if (ierr < 0)
		std::clog << *argv
			  << " failed to parse text (appendRow returned "
			  << ierr << ")\n" << inputrows[i] << std::endl;
	}

	ierr = ta->write(outdir, dsn,
			 "user-supplied data parsed by ardea.cpp");
	delete ta;
	if (ierr < 0) {
	    std::clog << *argv << " failed to write user-supplied data to "
		      << outdir << ", error code = " << ierr << std::endl;
	    return(ierr);
	}
    }
    else { // use hard-coded data and queries
	int64_t buf[] = {10, -21, 32, -43, 54, -65, 76, -87, 98, -127};

	ta->addColumn("s1", ibis::SHORT);
	ta->addColumn("i2", ibis::INT);
	ta->addColumn("b3", ibis::BYTE);
	ta->addColumn("l4", ibis::LONG);
	ta->addColumn("f5", ibis::FLOAT);
	ta->addColumn("d6", ibis::DOUBLE);
	ta->addColumn("k7", ibis::CATEGORY);
	ta->addColumn("t8", ibis::TEXT);
	ta->appendRow("1,2,3,4,5,6,7,8");
	ta->appendRow("2 3 4 5 6 7 8 9");
	ta->append("l4", 2, 5, buf);
	ta->append("s1", 3, 10, buf+2);
	ta->append("i2", 4, 10, buf+3);
	ta->append("b3", 10, 90, buf);
	ta->appendRow("10,11,12,13,14,15,16");
	ierr = ta->write(outdir, dsn, "test data written by ardea.cpp");
	delete ta;
	if (ierr < 0) {
	    std::clog << *argv << " failed to write data to " << outdir
		      << ", error code = " << ierr << std::endl;
	    return(ierr);
	}
    }

    ibis::table* tb = ibis::table::create(outdir);
    if (tb == 0) {
	std::cerr << *argv << " failed to reconstructure table from data "
	    "files in " << outdir << std::endl;
	return -10;
    }
    else if (! usersupplied && (tb->nRows() == 0 || tb->nColumns() != 8 ||
				tb->nRows() % 91 != 0)) {
	std::cerr << *argv << " data in " << outdir
		  << " is expected to have 8 "
	    "columns and a multiple of 91 rows, but it does not"
		  << std::endl;
    }
    if (ibis::gVerbose > 0) {
	std::cout << "-- begin printing table --\n";
	tb->describe(std::cout);
	if (tb->nRows() > 0 && tb->nColumns() > 0) {
	    if (ibis::gVerbose > 2) // print all values
		tb->dump(std::cout);
	    else // print the first ten rows
		printValues(*tb);
	}
	std::cout << "--  end  printing table --\n";
    }
    if (usersupplied == false) {
	// check the number of hits of built-in queries
	const char* arq[] = {"s1=1", "i2<=3", "l4<4",
			     "b3 between 10 and 100", "b3 > 0 && i2 < 0", 
			     "\"8\" == k7 or \"8\" == t8", "1+f5 == d6",
			     "s1 between 0 and 10 and i2 between 0 and 10",
			     "t8=a && l4 > 8", "sqrt(d6)+log(f5)<5 && b3 <0"};
	const size_t arc[] = {1, 7, 1, 6, 0, 2, 3, 2, 0, 0};
	const size_t multi = static_cast<size_t>(tb->nRows() / 91);
	ierr = 0;
	for (size_t i = 0; i < 10; ++ i) {
	    ibis::table* res = tb->select(static_cast<const char*>(0), arq[i]);
	    if (res == 0) {
		std::clog << "Query \"" << arq[i] << "\" on " << tb->name()
			  << " produced a null table" << std::endl;
	    }
	    else if (res->nRows() != multi*arc[i]) {
		std::clog << "Query \"" << arq[i]
			  << "\" is expected to produce "
			  << multi*arc[i] << " hit" << (multi*arc[i]>1?"s":"")
			  << ", but actual found " << res->nRows()
			  << std::endl;
		++ ierr;
	    }
	    else if (ibis::gVerbose > 0) {
		std::cout << "Query \"" << arq[i]
			  << "\" produced the expected number of hits ("
			  << res->nRows() << ")" << std::endl;
	    }
	    delete res;
	}
	std::cout << *argv << " processed 10 hard-coded queries on " << multi
		  << " cop" << (multi > 1 ? "ies" : "y")
		  << " of hard-coded data, found " << ierr
		  << " unexpected result" << (ierr > 1 ? "s" : "")
		  << std::endl;
    }
    // user-supplied queries
    for (qList::const_iterator qit = qcnd.begin();
	 qit != qcnd.end(); ++ qit) {
	doQuery(*tb, *qit, sel);
    }
    delete tb;
    return 0;
} // main