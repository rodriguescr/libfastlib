//File: $Id$
// Author: John Wu <John.Wu@ACM.org>
//         Lawrence Berkeley National Laboratory
// Copyright 2000-2008 the Regents of the University of California
#ifndef IBIS_INDEX_H
#define IBIS_INDEX_H
///@file
/// Definition of the common functions of an index.
///
/// The index class is a pure virtual base class with a static create
/// function and a few virtual functions that provide common functionality.
///
/// An index is built for each individual column (ibis::column) of a data
/// table.  The primary function of the index is to compute the solution or
/// an estimation (as a pair of upper and lower bounds) for a range query.
/// It needs to be generated and updated as necessary.  The simplest way of
/// generating an index is to build one from a file containing the binary
/// values of a column.  An index can only be updated for new records
/// appended to the data table.  Any other form of update, such as removal
/// of some records, change some existing records can only be updated by
/// removing the existing index then recreate the index.
///
/// 
#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)	// some identifier longer than 256 characters
#endif
#include "qExpr.h"
#include "bitvector.h"

#include <string>

namespace ibis { // the concrete classes of index hierarchy
    class bin;	 // equal size bins (equality encoded bitmap index)
    class range; // one-sided range encoded (cumulative) index
    class mesa;  // interval encoded index (two-sided ranges)
    class ambit; // two-level, cumulative index (cumulate on all levels)
    class pale;  // two-level, cumulative on the fine level only
    class pack;  // two-level, cumulative on the coarse level only
    class zone;  // two-level, do not cumulate on either levels
    class relic; // the basic bitmap index (one bitmap per distinct value)
    class slice; // the bit slice index (binary encoding of ibis::relic)
    class fade;  // a more controlled slice (multicomponent range code)
    class sbiad; // Italian translation of fade (multicomponent interval code)
    class sapid; // closest word to sbiad in English (multicomponent equality
		 // code)
    class egale; // French word for "equal" (multicomponent equality code on
		 // bins)
    class moins; // French word for "less" (multicomponent range code on bins)
    class entre; // French word for "in between" (multicomponent interval code
		 // on bins)
    class bak;   // Dutch word for "bin" (simple equality encoding for
		 // values mapped to reduced precision floating-point
		 // values)
    class bak2;  // a variation of bak that splits each bak bin in two
//     class waaier;// Dutch word for "range" (simple range encoding for values
// 		 // mapped to reduced precision floating-point values)
//     class tussen;// Dutch word for "in between" (simple interval encoding
// 		 // for values mapped to reduce precision floating-point
// 		 // values)
//     class uguale;// Italian word for "equal" (multicomponent version of bak)
//     class meno;  // Italian word for "less" (multicomponent version of waaier)
//     class fra;   // Italian word for "in between" (multicomponent version of
// 		 // tussen)
    class keywords;// A boolean version of term-document matrix.
    class mesh;  // Composite index on 2-D regular mesh
    class band;  // Composite index on 2-D bands.
    class direkte;// Directly use the integer values as bin numbers.
    class bylt;  // Unbinned version of pack.
    class zona;  // Unbinned version of zone.
    class fuzz;  // Unbinned version of interval-equality encoding.
    class fuge;  // Binned version of interval-equality encoding.
} // namespace ibis

/// The base index class.  Class ibis::index contains the common definitions
/// and virtual functions of the class hierarchy.  It is assumed that an
/// ibis::index is for only one column.  The user is to create an new index
/// through the function ibis::index::create and only use the functions
/// defined in this class.
class ibis::index {
public:
    // the integer value of this enum type are used in the index files to
    // differentiate the indices
    // **** reordering the list will make some index files invalid ****
    enum INDEX_TYPE {
	BINNING=0,	// bin
	RANGE,	// range
	MESA,	// interval
	AMBIT,	// range/range (coarse / fine)
	PALE,	// bin/range
	PACK,	// range/bin
	ZONE,	// bin/bin
	RELIC,	// basic bitmap index
	ROSTER,	// RID list
	SLICE,	// bit-sliced index (O'Neal)
	FADE,	// multicomponent range encoding
	SBIAD,	// multicomponent interval encoding
	SAPID,	// multicomponent equality encoding
	EGALE,	// multicomponent equality encoding on bins
	MOINS,	// multicomponent range encoding on bins
	ENTRE,	// multicomponent interval encoding on bins
	BAK,	// reduced precision mapping, equality code
	BAK2,	// splits each BAK bin in two, one less than
		// the mapped value, one greater and equal
		// to the mapped value
	KEYWORDS,	// boolean term-document matrix.
	MESH,	// an index on two attributes, bins form
		// regular mesh
	BAND,	// composite index of two attribute
	DIREKTE,// directly use the integer values as bin number
	GENERIC,// a generic index class template
	BYLT,	// unbinned range-equality encoding
	FUZZ,	// unbinned interval-equality encoding
	ZONA,	// unbinned equality-equality encoding
	FUGE	// binned interval-equality encoding
    };

    /// @brief Index factory.
    ///
    /// It creates a specific concrete index object.  If this function
    /// failed to read a specified index file, it will attempt to create a
    /// new index based on the current data file and index specification.
    /// The new index will be written under the old name.
    ///
    /// This function returns nil if it fails to create an index.
    ///
    /// @param c a pointer to a ibis::column object.  This argument must be
    /// present.
    ///
    /// @param name a name, it can be the name of the index file, the data
    /// file, or the directory containing the data file.  If the name ends
    /// with '.idx' is treated as an index file and the content of the file
    /// is read.  If the name does not end with '.idx', it is assumed to be
    /// the data file name unless it is determined to be directory name.  If
    /// it is a directory name, the data file is assumed to be in the
    /// directory with the file name same as the column name.  Once a data
    /// file is found, the content of the data file is read to construct a
    /// new index according to the return value of function indexSpec.  The
    /// argument name can be nil, in which case, the data file name is
    /// constructed by concatenate the return of
    /// partition()->currentDataDir() and the column name.
    ///
    /// @note Set @c name to null to build a brand new index and discard
    /// the existing index.
    ///
    /// @param spec the index specification.  This string contains the
    /// parameters for how to create an index.  The most general form is
    ///\verbatim
    /// <binning .../> <encoding .../> <compression .../>.
    ///\endverbatim
    /// Here is one example (it is the default for some integer columns)
    ///\verbatim
    /// <binning none /> <encoding equality />
    ///\endverbatim
    /// FastBit always compresses every bitmap it ever generates.  The
    /// compression option is to instruct it to uncompress some bitmaps or
    /// not compress indices at all.  The compress option is usually not
    /// used.
    ///
    /// If the argument @c spec is not specified, this function checks the
    /// specification in the following order.
    /// <ol>
    /// <li> use the index specification for the column being indexed;
    /// <li> use the index specification for the table containing the
    /// column being indexed;
    /// <li> use the most specific index specification relates to the
    /// column be indexed in the global resources (gParameters).
    /// </ol>
    /// It stops looking as soon as it finds the first non-empty string.
    /// To override any general index specification, one must provide a
    /// complete index specification string.
    static index* create(const column* c, const char* name=0,
			 const char* spec=0);
    /// Read the header of the named file to determine if it contains an
    /// index of the specified type.  Returns true if the correct header is
    /// found, else return false.
    static bool isIndex(const char* f, INDEX_TYPE t);
    /// The destructor.
    virtual ~index () {clear();};

    /// Returns an index type identifier.
    virtual INDEX_TYPE type() const = 0;
    /// Returns the name of the index, similar to the function @c type, but
    /// returns a string instead.
    virtual const char* name() const = 0;

    /// To evaluate the exact hits.  On success, return the number of hits,
    /// otherwise a negative value is returned.
    virtual long evaluate(const ibis::qContinuousRange& expr,
			  ibis::bitvector& hits) const = 0;
    /// To evaluate the exact hits.  On success, return the number of hits,
    /// otherwise a negative value is returned.
    virtual long evaluate(const ibis::qDiscreteRange&,
    			  ibis::bitvector&) const {return -1;}

    /// Computes an approximation of hits as a pair of lower and upper
    /// bounds.
    /// @param expr the query expression to be evaluated.
    /// @param lower a bitvector marking a subset of the hits.  All
    /// rows marked with one (1) are definitely hits.
    /// @param upper a bitvector marking a superset of the hits.  All
    /// hits are marked with one, but some of the rows marked one may not
    /// be hits.
    /// If the variable upper is empty, the variable lower is assumed to
    /// contain the exact answer.
    virtual void estimate(const ibis::qContinuousRange& expr,
			  ibis::bitvector& lower,
			  ibis::bitvector& upper) const = 0;
    /// Returns an upper bound on the number of hits.
    virtual uint32_t estimate(const ibis::qContinuousRange& expr) const = 0;
    /// Mark the position of the rows that can not be decided with this
    /// index.
    /// @param expr the range conditions to be evaluated.
    /// @param iffy the bitvector marking the positions of rows that can not
    /// be decided using the index.
    /// Return value is the expected fraction of undecided rows that might
    /// satisfy the range conditions.
    virtual float undecidable(const ibis::qContinuousRange& expr,
			      ibis::bitvector& iffy) const = 0;

    /// Estimate the hits for discrete ranges, i.e., those translated from
    /// 'a IN (x, y, ..)'.
    virtual void estimate(const ibis::qDiscreteRange& expr,
			  ibis::bitvector& lower,
			  ibis::bitvector& upper) const;
    virtual uint32_t estimate(const ibis::qDiscreteRange& expr) const;
    virtual float undecidable(const ibis::qDiscreteRange& expr,
			      ibis::bitvector& iffy) const;

    /// Estimate the pairs for the range join operator.
    virtual void estimate(const ibis::index& idx2,
			  const ibis::rangeJoin& expr,
			  ibis::bitvector64& lower,
			  ibis::bitvector64& upper) const;
    /// Estimate the pairs for the range join operator.  Only records that
    /// are masked are evaluated.
    virtual void estimate(const ibis::index& idx2,
			  const ibis::rangeJoin& expr,
			  const ibis::bitvector& mask,
			  ibis::bitvector64& lower,
			  ibis::bitvector64& upper) const;
    virtual void estimate(const ibis::index& idx2,
			  const ibis::rangeJoin& expr,
			  const ibis::bitvector& mask,
			  const ibis::qRange* const range1,
			  const ibis::qRange* const range2,
			  ibis::bitvector64& lower,
			  ibis::bitvector64& upper) const;
    /// Estimate an upper bound for the number of pairs.
    virtual int64_t estimate(const ibis::index& idx2,
			     const ibis::rangeJoin& expr) const;
    /// Estimate an upper bound for the number of pairs produced from
    /// marked records.
    virtual int64_t estimate(const ibis::index& idx2,
			     const ibis::rangeJoin& expr,
			     const ibis::bitvector& mask) const;
    virtual int64_t estimate(const ibis::index& idx2,
			     const ibis::rangeJoin& expr,
			     const ibis::bitvector& mask,
			     const ibis::qRange* const range1,
			     const ibis::qRange* const range2) const;

    /// Evaluating a join condition with one (likely composite) index.
    virtual void estimate(const ibis::rangeJoin& expr,
			  const ibis::bitvector& mask,
			  const ibis::qRange* const range1,
			  const ibis::qRange* const range2,
			  ibis::bitvector64& lower,
			  ibis::bitvector64& upper) const;
    virtual int64_t estimate(const ibis::rangeJoin& expr,
			     const ibis::bitvector& mask,
			     const ibis::qRange* const range1,
			     const ibis::qRange* const range2) const;

    /// Estimate the code of evaluate a range condition.
    virtual double estimateCost(const ibis::qContinuousRange& expr) const = 0;
    virtual double estimateCost(const ibis::qDiscreteRange& expr) const = 0;

    /// Prints human readable information.  Outputs information about the
    /// index as text to the specified output stream.
    virtual void print(std::ostream& out) const = 0;
    /// Save index to a file.  Outputs the index in a compact binary format
    /// to the named file or directory.  The index file contains a header
    /// that can be identified by the function isIndex.
    virtual void write(const char* name) const = 0;
    /// Reconstructs an index from the named file.  The name can be the
    /// directory containing an index file.  In this case, the name of the
    /// index file must be the name of the column followed by ".idx" suffix.
    virtual void read(const char* name) = 0;
    /// Reconstructs an index from an array of bytes.  Intended for internal
    /// use only!
    virtual void read(ibis::fileManager::storage* st) = 0;

    /// Extend the index.
    virtual long append(const char* dt, const char* df, uint32_t nnew) = 0;

    /// Time some logical operations and print out their speed.
    virtual void speedTest(std::ostream& out) const {};
    /// Returns the number of bit vectors used by the index.
    virtual uint32_t numBitvectors() const {return bits.size();}
    /// Return a pointer to the ith bitvector used in the index (may be 0).
    virtual const ibis::bitvector* getBitvector(uint32_t i) const {
	if (i < bits.size()) {
	    if (bits[i] == 0)
		activate(i);
	    return bits[i];
	}
	else {
	    return 0;
	}
    }

    /// The function binBoundaries and binWeights return bin boundaries and
    /// counts of each bin respectively.
    virtual void binBoundaries(std::vector<double>&) const = 0;
    virtual void binWeights(std::vector<uint32_t>&) const = 0;

    /// Cumulative distribution of the data.
    virtual long getCumulativeDistribution
    (std::vector<double>& bds, std::vector<uint32_t>& cts) const;
    /// Binned distribution of the data.
    virtual long getDistribution
    (std::vector<double>& bbs, std::vector<uint32_t>& cts) const;
    /// The minimum value recorded in the index.
    virtual double getMin() const = 0;
    /// The maximum value recorded in the index.
    virtual double getMax() const = 0;
    /// Compute the approximate sum of all the values indexed.  If it
    /// decides that computing the sum directly from the vertical partition
    /// is more efficient, it will return NaN immediately.
    virtual double getSum() const = 0;
    virtual uint32_t getNRows() const {return nrows;}

    /// The functions expandRange and contractRange expands or contracts the
    /// boundaries of a range condition so that the new range will have
    /// exact answers using the function estimate.  The default
    /// implementation provided does nothing since this is only meaningful
    /// for indices based on bins.
    virtual int expandRange(ibis::qContinuousRange& rng) const {return 0;}
    virtual int contractRange(ibis::qContinuousRange& rng) const {return 0;}

    typedef std::map< double, ibis::bitvector* > VMap;
    typedef std::map< double, uint32_t > histogram;
    template <typename E>
    static void mapValues(const array_t<E>& val, VMap& bmap);
    template <typename E>
    static void mapValues(const array_t<E>& val, histogram& hist,
			  uint32_t count=0);
    template <typename E>
    static void mapValues(const array_t<E>& val, array_t<E>& bounds,
			  std::vector<uint32_t>& cnts);
    template <typename E1, typename E2>
    static void mapValues(const array_t<E1>& val1, const array_t<E2>& val2,
			  array_t<E1>& bnd1, array_t<E2>& bnd2,
			  std::vector<uint32_t>& cnts);
    /// Determine how to split the array @c cnt, so that each group has
    /// roughly the same total value.
    static void divideCounts(array_t<uint32_t>& bounds,
			     const array_t<uint32_t>& cnt);

protected:
    // shared members for all indexes
    /// Pointer to the column this index is for.
    const ibis::column* col;
    /// The underlying storage.  It may be nil if bitvectors are not from a
    /// storage object managed by the file manager.
    mutable ibis::fileManager::storage* str;
    /// The name of the file containing the index.
    mutable const char* fname;
    /// Starting positions of the bitvectors.
    mutable array_t<int32_t> offsets;
    /// A list of bitvectors.
    mutable std::vector<ibis::bitvector*> bits;
    /// The number of rows represented by the index.
    uint32_t nrows;

    /// Protect the constructor so that ibis::index can not be instantiated
    /// directly.  It can not be instantiated because some functions are
    /// pure virtual, but this also reduces the size of public interface.
    index(const ibis::column* c=0, ibis::fileManager::storage* s=0)
	: col(c), str(s), fname(0), offsets(), nrows(0) {}

    /// Generate data file name from "f"
    void dataFileName(const char* f, std::string& name) const;
    /// Generate index file name from "f"
    void indexFileName(const char* f, std::string& name) const;
    static void indexFileName(std::string& name, const ibis::column* col1,
			      const ibis::column* col2, const char* f=0);

    /// Regenerate all bitvectors from the underlying storage.
    virtual void activate() const;
    /// Regenerate the ith bitvector from the underlying storage.
    virtual void activate(uint32_t i) const;
    /// Regenerate bitvectors i (inclusive) through j (exclusive) from the
    /// underlying storage. 
    virtual void activate(uint32_t i, uint32_t j) const;
    /// Clear the existing content
    virtual void clear();

    ////////////////////////////////////////////////////////////////////////
    // both VMap and histogram assume that all supported data types can be
    // safely stored in double
    ////////////////////////////////////////////////////////////////////////
    /// Map the positions of each individual value.
    void mapValues(const char* f, VMap& bmap) const;
    /// Generate a histogram.
    void mapValues(const char* f, histogram& hist, uint32_t count=0) const;
    void computeMinMax(const char* f, double& min, double& max) const;
    /// A function to decide whether to uncompress the bitvectors.
    void optionalUnpack(std::vector<ibis::bitvector*>& bits,
			const char* opt);
    /// Add the sum of @c bits[ib] through @c bits[ie-1] to @c res.  Always
    /// explicitly use @c bits[ib] through @c bits[ie-1].
    void addBits(uint32_t ib, uint32_t ie, ibis::bitvector& res) const;
    /// Compute the sum of bit vectors [@c ib, @c ie).  If computing a
    /// complement is faster, assume all bit vectors add up to @c tot.
    void addBits(uint32_t ib, uint32_t ie, ibis::bitvector& res,
		 const ibis::bitvector& tot) const;
    /// Compute the bitwise OR of all bitvectors (in bits) from ib to ie.
    /// As usual, bits[ib] is included but bits[ie] is excluded.
    void sumBits(uint32_t ib, uint32_t ie, ibis::bitvector& res) const;
    /// Compute a new sum for bit vectors [ib, ie) by taking advantage of the
    /// old sum for bitvectors [ib0, ie0).
    void sumBits(uint32_t ib, uint32_t ie, ibis::bitvector& res,
		 uint32_t ib0, uint32_t ie0) const;

    // three static functions to perform the task of summing up bit sequences
    static void addBins(const std::vector<ibis::bitvector*>& bits,
			uint32_t ib, uint32_t ie, ibis::bitvector& res);
    static void sumBins(const std::vector<ibis::bitvector*>& bits,
			uint32_t ib, uint32_t ie, ibis::bitvector& res);
    static void sumBins(const std::vector<ibis::bitvector*>& bits,
			const ibis::bitvector& tot, uint32_t ib, uint32_t ie,
			ibis::bitvector& res);
    // a static function to assign bases for multicomponent schemes
    static void setBases(array_t<uint32_t>& bases, uint32_t card,
			 uint32_t nbase = 2);

    class barrel;

private:
    index(const index&); // no copy constructor
    const index& operator=(const index&); // no assignment operator
}; // ibis::index


/// A specialization that adds function @c setValue.  This function allows
/// the client to directly set the value for an individual variable.
class ibis::index::barrel : public ibis::compRange::barrel {
public:
    barrel(const ibis::compRange::term* t) : ibis::compRange::barrel(t) {}

    void setValue(uint32_t i, double v) {varvalues[i] = v;}
}; // ibis::index::barrel
#endif // IBIS_INDEX_H