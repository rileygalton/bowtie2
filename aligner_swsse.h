/*
 * aligner_sse.h
 */

#ifndef ALIGNER_SWSSE_H_
#define ALIGNER_SWSSE_H_
#ifndef NO_SSE

#include "ds.h"
#include "mem_ids.h"
#include "random_source.h"
#include "scoring.h"
#include "mask.h"
#include <emmintrin.h>
#include <strings.h>

struct SSEMetrics {
	
	SSEMetrics() { reset(); }

	void clear() { reset(); }
	void reset() {
		dp = dpsat = dpfail = dpsucc = 
		col = cell = inner = fixup =
		gathcell = gathsol = bt = btfail = btsucc = btcell = 0;
	}
	
	void merge(const SSEMetrics& o, bool getLock = false) {
		ThreadSafe ts(&lock, getLock);
		dp       += o.dp;
		dpsat    += o.dpsat;
		dpfail   += o.dpfail;
		dpsucc   += o.dpsucc;
		col      += o.col;
		cell     += o.cell;
		inner    += o.inner;
		fixup    += o.fixup;
		gathcell += o.gathcell;
		gathsol  += o.gathsol;
		bt       += o.bt;
		btfail   += o.btfail;
		btsucc   += o.btsucc;
		btcell   += o.btcell;
	}

	uint64_t dp;       // DPs tried
	uint64_t dpsat;    // DPs saturated
	uint64_t dpfail;   // DPs failed
	uint64_t dpsucc;   // DPs succeeded
	uint64_t col;      // DP columns
	uint64_t cell;     // DP cells
	uint64_t inner;    // DP inner loop iters
	uint64_t fixup;    // DP fixup loop iters
	uint64_t gathcell; // DP gather cells examined
	uint64_t gathsol;  // DP gather solution cells found
	uint64_t bt;       // DP backtraces
	uint64_t btfail;   // DP backtraces failed
	uint64_t btsucc;   // DP backtraces succeeded
	uint64_t btcell;   // DP backtrace cells traversed
	MUTEX_T  lock;
};

/**
 * Encapsulates matrix information calculated by the SSE aligner.
 *
 * Matrix memory is laid out as follows:
 *
 * - Elements (individual cell scores) are packed into __m128i vectors
 * - Vectors are packed into quartets, quartet elements correspond to: a vector
 *   from E, one from F, one from H, and one that's "reserved"
 * - Quartets are packed into columns, where the number of quartets is
 *   determined by the number of query characters divided by the number of
 *   elements per vector
 *
 * Regarding the "reserved" element of the vector quartet: we use it for two
 * things.  First, we use the first column of reserved vectors to stage the
 * initial column of H vectors.  Second, we use the "reserved" vectors during
 * the backtrace procedure to store information about (a) which cells have been
 * traversed, (b) whether the cell is "terminal" (in local mode), etc.
 */
struct SSEMatrix {

	// Each matrix element is a quartet of vectors.  These constants are used
	// to identify members of the quartet.
	const static size_t E   = 0;
	const static size_t F   = 1;
	const static size_t H   = 2;
	const static size_t TMP = 3;

	SSEMatrix(int cat = 0) : nvecPerCell_(4), buf_(cat) { }

	/**
	 * Return a pointer to the matrix buffer.
	 */
	inline __m128i *ptr() {
		assert(inited_);
		return bufal_;
	}
	
	/**
	 * Return a pointer to the E vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* evec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + E;
	}

	/**
	 * Like evec, but it's allowed to ask for a pointer to one column after the
	 * final one.
	 */
	inline __m128i* evecUnsafe(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_leq(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + E;
	}

	/**
	 * Return a pointer to the F vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* fvec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + F;
	}

	/**
	 * Return a pointer to the H vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* hvec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + H;
	}

	/**
	 * Return a pointer to the TMP vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* tmpvec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + TMP;
	}

	/**
	 * Like tmpvec, but it's allowed to ask for a pointer to one column after
	 * the final one.
	 */
	inline __m128i* tmpvecUnsafe(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_leq(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + TMP;
	}
	
	/**
	 * Given a number of rows (nrow), a number of columns (ncol), and the
	 * number of words to fit inside a single __m128i vector, initialize the
	 * matrix buffer to accomodate the needed configuration of vectors.
	 */
	void init(
		size_t nrow,
		size_t ncol,
		size_t wperv);
	
	/**
	 * Return the number of __m128i's you need to skip over to get from one
	 * cell to the cell one column over from it.
	 */
	inline size_t colstride() const { return colstride_; }

	/**
	 * Return the number of __m128i's you need to skip over to get from one
	 * cell to the cell one row down from it.
	 */
	inline size_t rowstride() const { return rowstride_; }

	/**
	 * Given a row, col and matrix (i.e. E, F or H), return the corresponding
	 * element.
	 */
	int eltSlow(size_t row, size_t col, size_t mat) const;
	
	/**
	 * Given a row, col and matrix (i.e. E, F or H), return the corresponding
	 * element.
	 */
	inline int elt(size_t row, size_t col, size_t mat) const {
		assert(inited_);
		assert_lt(row, nrow_);
		assert_lt(col, ncol_);
		assert_lt(mat, 3);
		// Move to beginning of column/row
		size_t rowelt = row / nvecrow_;
		size_t rowvec = row % nvecrow_;
		size_t eltvec = (col * colstride_) + (rowvec * rowstride_) + mat;
		if(wperv_ == 16) {
			return (int)((uint8_t*)&bufal_[eltvec])[rowelt];
		} else {
			assert_eq(8, wperv_);
			return (int)((int16_t*)&bufal_[eltvec])[rowelt];
		}
	}

	/**
	 * Given a row, col and matrix (i.e. E, F or H), return the corresponding
	 * element.
	 */
	inline void* eltptr(size_t row, size_t col, size_t mat) const {
		assert(inited_);
		assert_lt(row, nrow_);
		assert_lt(col, ncol_);
		assert_lt(mat, 3);
		// Move to beginning of column/row
		size_t rowelt = row / nvecrow_;
		size_t rowvec = row % nvecrow_;
		size_t eltvec = (col * colstride_) + (rowvec * rowstride_) + mat;
		return &bufal_[eltvec] + rowelt;
	}
	
	/**
	 * Return the element in the E matrix at element row, col.
	 */
	inline int eelt(size_t row, size_t col) const {
		return elt(row, col, E);
	}

	/**
	 * Return the element in the F matrix at element row, col.
	 */
	inline int felt(size_t row, size_t col) const {
		return elt(row, col, F);
	}

	/**
	 * Return the element in the H matrix at element row, col.
	 */
	inline int helt(size_t row, size_t col) const {
		return elt(row, col, H);
	}
	
	/**
	 * Return true iff the given cell has its reportedThru bit set.
	 */
	inline bool reportedThrough(
		size_t row,          // current row
		size_t col) const    // current column
	{
		return ((masks_[row * ncol_ + col] & (1 << 0)) != 0);
	}

	/**
	 * Set the given cell's reportedThru bit.
	 */
	inline void setReportedThrough(
		size_t row,          // current row
		size_t col)          // current column
	{
		masks_[row * ncol_ + col] |= (1 << 0);
	}

	/**
	 * Return true iff the H mask has been set with a previous call to hMaskSet.
	 */
	bool isHMaskSet(
		size_t row,          // current row
		size_t col) const;   // current column

	/**
	 * Set the given cell's H mask.  This is the mask of remaining legal ways to
	 * backtrack from the H cell at this coordinate.  It's 5 bits long and has
	 * offset=2 into the 16-bit field.
	 */
	void hMaskSet(
		size_t row,          // current row
		size_t col,          // current column
		int mask);

	/**
	 * Return true iff the E mask has been set with a previous call to eMaskSet.
	 */
	bool isEMaskSet(
		size_t row,          // current row
		size_t col) const;   // current column

	/**
	 * Set the given cell's E mask.  This is the mask of remaining legal ways to
	 * backtrack from the E cell at this coordinate.  It's 2 bits long and has
	 * offset=8 into the 16-bit field.
	 */
	void eMaskSet(
		size_t row,          // current row
		size_t col,          // current column
		int mask);
	
	/**
	 * Return true iff the F mask has been set with a previous call to fMaskSet.
	 */
	bool isFMaskSet(
		size_t row,          // current row
		size_t col) const;   // current column

	/**
	 * Set the given cell's F mask.  This is the mask of remaining legal ways to
	 * backtrack from the F cell at this coordinate.  It's 2 bits long and has
	 * offset=11 into the 16-bit field.
	 */
	void fMaskSet(
		size_t row,          // current row
		size_t col,          // current column
		int mask);

	/**
	 * Analyze a cell in the SSE-filled dynamic programming matrix.  Determine &
	 * memorize ways that we can backtrack from the cell.  If there is at least one
	 * way to backtrack, select one at random and return the selection.
	 *
	 * There are a few subtleties to keep in mind regarding which cells can be at
	 * the end of a backtrace.  First of all: cells from which we can backtrack
	 * should not be at the end of a backtrace.  But have to distinguish between
	 * cells whose masks eventually become 0 (we shouldn't end at those), from
	 * those whose masks were 0 all along (we can end at those).
	 */
	void analyzeCell(
		size_t row,          // current row
		size_t col,          // current column
		size_t ct,           // current cell type: E/F/H
		int refc,
		int readc,
		int readq,
		const Scoring& sc,   // scoring scheme
		TAlScore offsetsc,   // offset to add to each score
		TAlScore floorsc,    // local-alignment score floor
		RandomSource& rand,  // rand gen for choosing among equal options
		bool& empty,         // out: =true iff no way to backtrace
		int& cur,            // out: =type of transition
		bool& branch,        // out: =true iff we chose among >1 options
		bool& canMoveThru,   // out: =true iff ...
		bool& reportedThru); // out: =true iff ...

	/**
	 * Initialize the matrix of masks and backtracking flags.
	 */
	void initMasks();

	/**
	 * Return the number of rows in the dynamic programming matrix.
	 */
	size_t nrow() const {
		return nrow_;
	}

	/**
	 * Return the number of columns in the dynamic programming matrix.
	 */
	size_t ncol() const {
		return ncol_;
	}

	bool             inited_;      // initialized?
	size_t           nrow_;        // # rows
	size_t           ncol_;        // # columns
	size_t           nvecrow_;     // # vector rows (<= nrow_)
	size_t           nveccol_;     // # vector columns (<= ncol_)
	size_t           wperv_;       // # words per vector
	size_t           vecshift_;    // # bits to shift to divide by words per vec
	size_t           nvecPerCol_;  // # vectors per column
	size_t           nvecPerCell_; // # vectors per matrix cell (4)
	size_t           colstride_;   // # vectors b/t adjacent cells in same row
	size_t           rowstride_;   // # vectors b/t adjacent cells in same col
	EList<__m128i>   buf_;         // buffer for holding vectors
	EList<uint16_t>  masks_;       // buffer for masks/backtracking flags
	__m128i         *bufal_;       // 16-byte aligned version of the ptr for buf_
};

/**
 * All the data associated with the query profile and other data needed for SSE
 * alignment of a query.
 */
struct SSEData {
	SSEData(int cat = 0) : buf_(cat), mat_(cat) { }
	EList<uint8_t> buf_;         // buffer for query profile & temp vecs
	__m128i       *qprof_;       // query profile
	size_t         qprofStride_; // stride for query profile
	size_t         gbarStride_;  // gap barrier for query profile
	SSEMatrix      mat_;         // SSE matrix for holding all E, F, H vectors
	size_t         maxPen_;      // biggest penalty of all
	size_t         maxBonus_;    // biggest bonus of all
	size_t         lastIter_;    // which 128-bit striped word has final row?
	size_t         lastWord_;    // which word within 128-word has final row?
	int            bias_;        // all scores shifted up by this for unsigned
};

#if 0
/**
 * Analyze a cell in the SSE-filled dynamic programming matrix.  Determine &
 * memorize ways that we can backtrack from the cell.  If there is at least one
 * way to backtrack, select one at random and return the selection.
 *
 * There are a few subtleties to keep in mind regarding which cells can be at
 * the end of a backtrace.  First of all: cells from which we can backtrack
 * should not be at the end of a backtrace.  But have to distinguish between
 * cells whose masks eventually become 0 (we shouldn't end at those), from
 * those whose masks were 0 all along (we can end at those).
 */
inline void SSEMatrix::analyzeCell(
	size_t row,          // current row
	size_t col,          // current column
	size_t ct,           // current cell type: E/F/H
	int refc,
	int readc,
	int readq,
	const Scoring& sc,   // scoring scheme
	TAlScore offsetsc,   // add to matrix elements to get actual score
	TAlScore floorsc,    // local-alignment score floor
	RandomSource& rand,  // rand gen for choosing among equal options
	bool& empty,         // out: =true iff no way to backtrace
	int& cur,            // out: =type of transition
	bool& branch,        // out: =true iff we chose among >1 options
	bool& canMoveThru,   // out: =true iff ...
	bool& reportedThru)  // out: =true iff ...
{
	TAlScore sc_cur;
	reportedThru = reportedThrough(row, col);
	canMoveThru = true;
	if(reportedThru) {
		canMoveThru = false;
		return;
	}
	empty = false;
	if(row == 0) {
		return;
	}
	assert_gt(row, 0);
	size_t rowFromEnd = nrow_ - row - 1;
	bool gapsAllowed = true;
	if(row < (size_t)sc.gapbar || rowFromEnd < (size_t)sc.gapbar) {
		gapsAllowed = false;
	}
	if(ct == E) { // AKA rdgap
		sc_cur = eelt(row, col) + offsetsc;
		assert(gapsAllowed);
		// Currently in the E matrix; incoming transition must come from the
		// left.  It's either a gap open from the H matrix or a gap extend from
		// the E matrix.
		assert_gt(col, 0);
		// TODO: save and restore origMask as well as mask
		int origMask = 0, mask = 0;
		// Get H score of cell to the left
		TAlScore sc_h_left = helt(row, col-1) + offsetsc;
		if(sc_h_left > floorsc && sc_h_left - sc.readGapOpen() == sc_cur) {
			mask |= (1 << 0);
		}
		// Get E score of cell to the left
		TAlScore sc_e_left = eelt(row, col-1) + offsetsc;
		if(sc_e_left > floorsc && sc_e_left - sc.readGapExtend() == sc_cur) {
			mask |= (1 << 1);
		}
		origMask = mask;
		assert(origMask > 0 || sc_cur <= sc.match());
		if(isEMaskSet(row, col)) {
			mask = (masks_[row * ncol_ + col] >> 8) & 3;
		}
		if(mask == 3) {
			if(rand.nextU2()) {
				// I chose the H cell
				cur = SW_BT_OALL_READ_OPEN;
				eMaskSet(row, col, 2); // might choose E later
			} else {
				// I chose the E cell
				cur = SW_BT_RDGAP_EXTEND;
				eMaskSet(row, col, 1); // might choose H later
			}
			branch = true;
		} else if(mask == 2) {
			// I chose the E cell
			cur = SW_BT_RDGAP_EXTEND;
			eMaskSet(row, col, 0); // done
		} else if(mask == 1) {
			// I chose the H cell
			cur = SW_BT_OALL_READ_OPEN;
			eMaskSet(row, col, 0); // done
		} else {
			empty = true;
			// It's empty, so the only question left is whether we should be
			// allowed in terimnate in this cell.  If it's got a valid score
			// then we *shouldn't* be allowed to terminate here because that
			// means it's part of a larger alignment that was already reported.
			canMoveThru = (origMask == 0);
		}
		assert(!empty || !canMoveThru);
	} else if(ct == F) { // AKA rfgap
		sc_cur = felt(row, col) + offsetsc;
		assert(gapsAllowed);
		// Currently in the F matrix; incoming transition must come from above.
		// It's either a gap open from the H matrix or a gap extend from the F
		// matrix.
		assert_gt(row, 0);
		// TODO: save and restore origMask as well as mask
		int origMask = 0, mask = 0;
		// Get H score of cell above
		TAlScore sc_h_up = helt(row-1, col) + offsetsc;
		if(sc_h_up > floorsc && sc_h_up - sc.refGapOpen() == sc_cur) {
			mask |= (1 << 0);
		}
		// Get F score of cell above
		TAlScore sc_f_up = felt(row-1, col) + offsetsc;
		if(sc_f_up > floorsc && sc_f_up - sc.refGapExtend() == sc_cur) {
			mask |= (1 << 1);
		}
		origMask = mask;
		assert(origMask > 0 || sc_cur <= sc.match());
		if(isFMaskSet(row, col)) {
			mask = (masks_[row * ncol_ + col] >> 11) & 3;
		}
		if(mask == 3) {
			if(rand.nextU2()) {
				// I chose the H cell
				cur = SW_BT_OALL_REF_OPEN;
				fMaskSet(row, col, 2); // might choose E later
			} else {
				// I chose the F cell
				cur = SW_BT_RFGAP_EXTEND;
				fMaskSet(row, col, 1); // might choose E later
			}
			branch = true;
		} else if(mask == 2) {
			// I chose the F cell
			cur = SW_BT_RFGAP_EXTEND;
			fMaskSet(row, col, 0); // done
		} else if(mask == 1) {
			// I chose the H cell
			cur = SW_BT_OALL_REF_OPEN;
			fMaskSet(row, col, 0); // done
		} else {
			empty = true;
			// It's empty, so the only question left is whether we should be
			// allowed in terimnate in this cell.  If it's got a valid score
			// then we *shouldn't* be allowed to terminate here because that
			// means it's part of a larger alignment that was already reported.
			canMoveThru = (origMask == 0);
		}
		assert(!empty || !canMoveThru);
	} else {
		sc_cur = helt(row, col) + offsetsc;
		assert_eq(H, ct);
		// TODO: save and restore origMask as well as mask
		int origMask = 0, mask = 0;
		TAlScore sc_f_up     = felt(row-1, col) + offsetsc;
		TAlScore sc_h_up     = helt(row-1, col) + offsetsc;
		TAlScore sc_h_left   = col > 0 ? (helt(row, col-1) + offsetsc) : floorsc;
		TAlScore sc_e_left   = col > 0 ? (eelt(row, col-1) + offsetsc) : floorsc;
		TAlScore sc_h_upleft = col > 0 ? (helt(row-1, col-1) + offsetsc) : floorsc;
		TAlScore sc_diag     = sc.score(readc, (int)refc, readq - 33);
		if(gapsAllowed) {
			if(sc_h_up     > floorsc && sc_cur == sc_h_up   - sc.refGapOpen()) {
				mask |= (1 << 0);
			}
			if(sc_h_left   > floorsc && sc_cur == sc_h_left - sc.readGapOpen()) {
				mask |= (1 << 1);
			}
			if(sc_f_up     > floorsc && sc_cur == sc_f_up   - sc.refGapExtend()) {
				mask |= (1 << 2);
			}
			if(sc_e_left   > floorsc && sc_cur == sc_e_left - sc.readGapExtend()) {
				mask |= (1 << 3);
			}
		}
		if(sc_h_upleft > floorsc && sc_cur == sc_h_upleft + sc_diag) {
			mask |= (1 << 4);
		}
		origMask = mask;
		assert(origMask > 0 || sc_cur <= sc.match());
		if(isHMaskSet(row, col)) {
			mask = (masks_[row * ncol_ + col] >> 2) & 31;
		}
		assert(gapsAllowed || mask == (1 << 4) || mask == 0);
		int opts = alts5[mask];
		int select = -1;
		if(opts == 1) {
			select = firsts5[mask];
			assert_geq(mask, 0);
			hMaskSet(row, col, 0);
		} else if(opts > 1) {
			select = randFromMask(rand, mask);
			assert_geq(mask, 0);
			mask &= ~(1 << select);
			assert(gapsAllowed || mask == (1 << 4) || mask == 0);
			hMaskSet(row, col, mask);
			branch = true;
		} else { /* No way to backtrack! */ }
		if(select != -1) {
			if(select == 4) {
				cur = SW_BT_OALL_DIAG;
			} else if(select == 0) {
				cur = SW_BT_OALL_REF_OPEN;
			} else if(select == 1) {
				cur = SW_BT_OALL_READ_OPEN;
			} else if(select == 2) {
				cur = SW_BT_RFGAP_EXTEND;
			} else {
				assert_eq(3, select)
				cur = SW_BT_RDGAP_EXTEND;
			}
		} else {
			empty = true;
			// It's empty, so the only question left is whether we should be
			// allowed in terimnate in this cell.  If it's got a valid score
			// then we *shouldn't* be allowed to terminate here because that
			// means it's part of a larger alignment that was already reported.
			canMoveThru = (origMask == 0);
		}
	}
	assert(!empty || !canMoveThru || ct == H);
	
#if 0
	if(empty) {
		cout << "EMPTY" << endl;
	} else {
		if(cur == SW_BT_OALL_DIAG) {
			cout << "H -> diag -> H from [" << row << ", " << col << "]" << endl;
		}
		if(cur == SW_BT_OALL_REF_OPEN) {
			cout << "H -> ref open -> H from [" << row << ", " << col << "]" << endl;
		}
		if(cur == SW_BT_OALL_READ_OPEN) {
			cout << "H -> read open -> H from [" << row << ", " << col << "]" << endl;
		}
		if(cur == SW_BT_RFGAP_EXTEND) {
			cout << "F -> ref extend -> H from [" << row << ", " << col << "]" << endl;
		}
		if(cur == SW_BT_RDGAP_EXTEND) {
			cout << "E -> read extend -> H from [" << row << ", " << col << "]" << endl;
		}
		if(branch) {
			cout << "  BRANCHED" << endl;
		}
	}
#endif
}
#endif

/**
 * Return true iff the H mask has been set with a previous call to hMaskSet.
 */
inline bool SSEMatrix::isHMaskSet(
	size_t row,          // current row
	size_t col) const    // current column
{
	return ((masks_[row * ncol_ + col] & (1 << 1)) != 0);
}

/**
 * Set the given cell's H mask.  This is the mask of remaining legal ways to
 * backtrack from the H cell at this coordinate.  It's 5 bits long and has
 * offset=2 into the 16-bit field.
 */
inline void SSEMatrix::hMaskSet(
	size_t row,          // current row
	size_t col,          // current column
	int mask)
{
	assert_lt(mask, 32);
	masks_[row * ncol_ + col] &= ~(31 << 1);
	masks_[row * ncol_ + col] |= (1 << 1 | mask << 2);
}

/**
 * Return true iff the E mask has been set with a previous call to eMaskSet.
 */
inline bool SSEMatrix::isEMaskSet(
	size_t row,          // current row
	size_t col) const    // current column
{
	return ((masks_[row * ncol_ + col] & (1 << 7)) != 0);
}

/**
 * Set the given cell's E mask.  This is the mask of remaining legal ways to
 * backtrack from the E cell at this coordinate.  It's 2 bits long and has
 * offset=8 into the 16-bit field.
 */
inline void SSEMatrix::eMaskSet(
	size_t row,          // current row
	size_t col,          // current column
	int mask)
{
	assert_lt(mask, 4);
	masks_[row * ncol_ + col] &= ~(7 << 7);
	masks_[row * ncol_ + col] |=  (1 << 7 | mask << 8);
}

/**
 * Return true iff the F mask has been set with a previous call to fMaskSet.
 */
inline bool SSEMatrix::isFMaskSet(
	size_t row,          // current row
	size_t col) const    // current column
{
	return ((masks_[row * ncol_ + col] & (1 << 10)) != 0);
}

/**
 * Set the given cell's F mask.  This is the mask of remaining legal ways to
 * backtrack from the F cell at this coordinate.  It's 2 bits long and has
 * offset=11 into the 16-bit field.
 */
inline void SSEMatrix::fMaskSet(
	size_t row,          // current row
	size_t col,          // current column
	int mask)
{
	assert_lt(mask, 4);
	masks_[row * ncol_ + col] &= ~(7 << 10);
	masks_[row * ncol_ + col] |=  (1 << 10 | mask << 11);
}

#endif /*ndef NO_SSE*/
#endif /*ndef ALIGNER_SWSSE_H_*/