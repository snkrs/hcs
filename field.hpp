#pragma once
/*
 * field.hpp
 *
 * Sparse storage in HCS
 *
 * This is a sparse storage class for the H coordinate system.
 * - dedicated refinement / coarsening
 * - only complete "H"s exist
 * - lower-level coords always exist, but top-level get marked as such. (Top-Level-Coordinate TLC)
 * - iterator class that allows fast iteration over all top-level or all existing coords or
 * 	 existing coords of a specific level
 * - bi-linear interpolation of non-existing coords, providing coefficients for TLC
 * - Arbitrary data type that needs to support some basic arithmetic
 * - Field supplies basic arithmetic operators
 * - A bracket operator for coordinates is implemented, with adjustable behavior for non-existing coords.
 * - The performance of exists() relies on STL's map::lower_bound O(log) complexity
 * - The center coordinate (0) always exists
 * - boundary conditions can be implemented as lambdas
 *
 */

using namespace std;
using namespace hcs;

template <typename DTYPE, typename HCSTYPE>
class Field {

public:

    Field(HCSTYPE hcs_) : hcs(hcs_), bracket_behavior(BR_INTERP) {
        for (auto &bf : boundary)
            bf = nullptr;
        for (bool &bf_prop : boundary_propagate)
            bf_prop = true;
    }

    Field() : Field(HCSTYPE()) {}

    virtual ~Field() {}

    // Any other type of Field is a friend.
    template <typename U, typename V>
    friend class Field;

    // The H-coordinate system to operate on
    HCSTYPE	hcs;

    // The boundary functions
    array<function<DTYPE(Field<DTYPE, HCSTYPE> *self, coord_t origin)>, 64> boundary; // max 32 dimensions
    array<bool, 64> boundary_propagate;												  // if this field is copied, is the boundary function copied too?

    // If a value is accessed via [], and if that value does not exist:
    //   BR_THROW: throws range_error, slow if it happens often.
    //   BR_REFINE: brings requested coord into existence via refineToCoord(), might be very slow
    //   BR_INTERP: useful if you only read from the coord. The intermediate is filled with the interpolated value (via get()).
    //				writing to the returned reference just sets the intermediate.
    //   BR_NOTHING: Just return a reference to the intermediate. Fastest version.
    //				Set intermediate to a value that marks non-existence and check return...
    //				writing to the returned reference just sets the intermediate.
    enum { BR_THROW, BR_INTERP, BR_NOTHING, BR_REFINE } bracket_behavior;

    //  Used as reference for the [] operator if coord does not exist, see above
    DTYPE		intermediate;

    //  The type to store a list of coords and their coefficients.
    //  It is a map instead of a vector because of unique coord elimination.
    typedef map<coord_t, data_t> coeff_map_t;


public:

    // This is the (strictly forward) iterator class that needs to be implemented
    class CustomIterator {
    public:
        CustomIterator() : at_end(true) {}
        virtual void increment() { cerr << "CI: INC CALLED\n"; throw bad_function_call();};
        virtual pair<coord_t, DTYPE&> getCurrentPair() { cerr << "CI: GET CALLED\n"; throw bad_function_call();};
        virtual CustomIterator* clone() {  cerr << "CI: CLONE CALLED\n"; throw bad_function_call();};
        bool at_end;
    };

    // NEVER overwrite this class.
    class Iterator {
    private:
        CustomIterator* ci;
    public:
        Iterator() : ci() {}
        Iterator(CustomIterator* ci) : ci(ci) {}
        Iterator(Iterator const& right) : ci(right.ci->clone()) {}

        ~Iterator() { delete ci; }

        Iterator& operator=(Iterator const& right) {
            delete ci;
            ci = right.ci->clone();
            return *this;
        }
        // these three methods form the basis of an iterator for use with
        // a range-based for loop
        bool operator!= (const Iterator& other) const {
            return !ci->at_end;
        }

        pair<coord_t, DTYPE&> operator* () const { return ci->getCurrentPair();};
        pair<coord_t, DTYPE&>* operator-> () const { return &ci->getCurrentPair();};

        Iterator& operator++ () { ci->increment(); return *this;};
    };

    // Iterator methods & class
    virtual Iterator begin(bool top_only = false, int only_level = -1) = 0;
    virtual Iterator end() = 0;

    // Returns the number of available elements for this field
    virtual size_t nElements() = 0;

    // Returns the number of top-level elements for this field
    virtual size_t nElementsTop() = 0;

    // Read-write access to existing coords. For (probably) non-existing, use get() and retrieve interpolated values.
    // the value set to bracket_behavior applies.
    virtual DTYPE& operator[](coord_t coord) = 0;

    // Do we have a value for this coord? And if yes, make sure it is in _current
    // A bucket's end coord is its last existing coord
    virtual bool exists(coord_t coord) = 0;

    // Does not query coefficients, throws if coord does not exist
    virtual DTYPE& getDirect(coord_t coord) = 0;

    // Returns value for coord, if not present, interpolates.
    // if it is not TLC, return value anyway. To retrieve proper values from non-TLC
    // call propagate() first
    virtual DTYPE get(coord_t coord, bool use_non_top = true) = 0;

    void get(coord_t coord, DTYPE& result, bool use_non_top = true) {
        if (hcs.IsBoundary(coord)) {
            uint8_t boundary_index = hcs.GetBoundaryDirection(coord);
            if (boundary[boundary_index] != nullptr)
                result = boundary[boundary_index](this, coord);
            else
                result = 0;
            return;
        }
        if (exists(coord)) {
            if (use_non_top || isTop(coord)) {
                result += getDirect(coord);
                return;
            } else {
                for (uint16_t direction = 0; direction < hcs.parts; direction++) {
                    //coeff_up_count++;
                    DTYPE partial = 0;
                    //getCoeffs(hcs.IncreaseLevel(coord, direction), partial, use_non_top, recursion + 1);
                    get(hcs.IncreaseLevel(coord, direction), partial, use_non_top);
                    partial /= (data_t)hcs.parts;
                    result += partial;
                }
            }
        } else {

            uint16_t high_part = hcs.extract(coord, 0);
            coord_t origin = hcs.ReduceLevel(coord);

            array<bool, 64> boundary_quench;

            for (uint8_t j = 0; j < hcs.GetDimensions(); j++) {
                bool plus = ((high_part >> j) & 1);
                boundary_quench[j] = hcs.IsBoundary(hcs.getNeighbor(origin, 2 * j + (plus ? 0 : 1)));
            }

            for (uint8_t i = 0; i <= hcs.part_mask; i++) {
                coord_t current = origin;

                data_t weight = 1;

                for (uint8_t j = 0; j < hcs.GetDimensions(); j++)
                    weight *= boundary_quench[j] ? 0.5 : (((i >> j) & 1) ? 0.25 : 0.75);

                set<coord_t> boundary_shares;

                for (uint8_t j = 0; j < hcs.GetDimensions(); j++) {
                    if (((i >> j) & 1) == 0)
                        continue;

                    coord_t prev_current = current;
                    current = hcs.getNeighbor(current, 2 * j + (((high_part >> j) & 1) ? 0 : 1));
                    if (hcs.IsBoundary(current)) {
                        boundary_shares.insert(current);
                        current = prev_current;
                    }
                }


                // get coeffs for current

                if (!boundary_shares.empty()) {
                    for (auto b_coord : boundary_shares) {
                        uint8_t boundary_index = hcs.GetBoundaryDirection(b_coord);
                        if (boundary[boundary_index] != nullptr)
                            result += boundary[boundary_index](this, b_coord) * (weight / (data_t)boundary_shares.size()); // Ask the provided boundary callback
                    }
                    continue;
                }

                bool current_exists = exists(current);
                if (!current_exists || (current_exists && !isTop(current) && !use_non_top)) {
                    // we either have a non-existent coord or an existing non-top coord that we shall not use.
                    DTYPE partial = 0;
                    //coeff_down_count++;
                    get(current, partial, use_non_top);
                    //upscale_cache[current] = partial;
                    result += partial * weight;
                } else { // current_exists = true in this branch, so _current is valid.
                    result += getDirect(current) * weight;
                }
            }
        }
    }

    // Do coordinates exist in a higher level?
    virtual bool isTop(coord_t coord) = 0;	// Average all non-top coords from top-level

    // Propagates values down from top-level to lowest level by averaging them.
    // If there would be a reverse iterator, a generic algorithm would be possible here...
    virtual void propagate() = 0;

    // Return interpolation coeffs and their associated >existing< coords.
    // The first value of the pair is the coefficient, always >0 and <=1.
    // If the coord exists the returning vector will be of size 1 and first=1., second=coord.
    // use_non_top = true uses coefficients from existing, but not top-level coordinates. Use propagate() first
    // to set non-top values to their averaged versions from top-level values.
    // never use recursion parameter, its purely internal to protect the stack.
    void getCoeffs(const coord_t coord, coeff_map_t &coeffs, bool use_non_top = true, int recursion = 0) {
        if (hcs.IsBoundary(coord)) {
            coeffs[coord] = 1.;
            return;
        }
        if (recursion > hcs.max_level) {
            cout << "RECURSION LIMIT REACHED (" << hcs.max_level << ") coord: " << hcs.toString(coord) << endl;
            exit(1);
        }
        if (exists(coord)) {
            if (isTop(coord) || use_non_top) {
                coeffs[coord] = 1.;
                return;
            } else {
                for (uint16_t direction = 0; direction < hcs.parts; direction++) {
                    coeff_map_t partial;
                    getCoeffs(hcs.IncreaseLevel(coord, direction), partial, use_non_top, recursion + 1);
                    for (auto &coeff : partial)
                        coeff.second /= hcs.parts;
                    coeffs.insert(partial.begin(), partial.end());
                }
            }
        } else {

            //typename HCSTYPE::neighbor_t ne;
            // Spawn a rectangle of lower-level coords around missing coord
            // A (hyper)cubical interpolation (2D bi-linear, 3D tri-linear,...) is the best choice,
            // simplexes (triangle, tetrahedron, ...) are not unique in orthogonal spaced coordinates.
            // The neighborhood-search that returns 2^D coordinates that cover our coord is
            // surprisingly straight forward, and the interpolation factors follow the same schema.
            // The originating coord is the one from reducing coord. It is always our closest corner,
            // should therefore get the highest interpolation factor.
            // From there the high_part of coord determines the first D search directions.
            // Hypercube search pattern that surrounds coord from a lower level:
            // 2D: Requires 4 coords (box). The first is _aways_ the level-reduced version of
            //	   coord itself, others are determined by the reduced direction (high_part) of coord.
            // high_part = 0b11 -> X+ Y+ (X+)Y+ <<- SAME ->> (Y+)X+  = 3 neighbors
            // 			   0b00 -> X- Y- (X-)Y- <<- SAME ->> (Y-)X-
            // 			   0b01 -> X+ Y- (X+)Y- <<- SAME ->> (Y-)X+
            // Coords 3D : Box with 8 corners, one is known.
            // 0b101 -> X+ Y- Z+ (X+)Y- (X+)Z+ (Y-)Z+ ((X+)Y-)Z+
            //        The order is not important. Many combinations lead to the same coord.
            //		  This combination follows a bit-order from ordinary counting!
            //		  Three bits for three dimensions, the order is not important, the
            //		  neighborhood direction from high_part is!
            //        X+ Y- Z+
            //        0  0  0     (nothing, the origin point)
            //        0  0  1     Z+
            //        0  1  0     Y-
            //		  0  1  1     Y- -> Z+
            //	      1  0  0     X+
            //		  1  0  1     X+ -> Z+
            //        1  1  0     X+ -> Y-  (the neighbor of X+ in Y- direction)
            //		  1  1  1     X+ -> Y- -> Z+ (the one on the opposite site)
            // Weights 3D:
            //     0 = 0.75, 1=0.25
            //        0  0  0  =  0.75³         = 0.4219
            //        0  0  1  =  0.25  * 0.75² = 0.1406
            //        0  1  0  =  0.25  * 0.75² = 0.1406
            //        0  1  1  =  0.25² * 0.75  = 0.0469
            //		  1  0  0  =  0.25  * 0.75² = 0.1406
            //        1  0  1  =  0.25² * 0.75  = 0.0469
            //        1  1  0  =  0.25² * 0.75  = 0.0469
            //        1  1  1  =  0.25³         = 0.0156
            //						TOTAL	    = 1 :)
            // This principle is universal for all dimensions!
            uint16_t high_part = hcs.extract(coord, 0); 	//
            coord_t origin = hcs.ReduceLevel(coord);
            //hcs.getNeighbors(origin, ne);
            array<bool, 64> boundary_quench;


            for (uint8_t j = 0; j < hcs.GetDimensions(); j++) {
                bool plus = ((high_part >> j) & 1);
                boundary_quench[j] = hcs.IsBoundary(hcs.getNeighbor(origin, 2 * j + (plus ? 0 : 1)));
            }


            array<tuple<coord_t, data_t, int>, 64> collection;
            for (uint8_t i = 0; i <= hcs.part_mask; i++) {
                coord_t current = origin;
                std::get<2>(collection[i]) = 0;
                int boundaries_involved = 0;
                vector<coord_t> bc_collector;

                data_t weight = 1;

                for (uint8_t j = 0; j < hcs.GetDimensions(); j++)
                    weight *= boundary_quench[j] ? 0.5 : (((i >> j) & 1) ? 0.25 : 0.75);


                for (uint8_t j = 0; j < hcs.GetDimensions(); j++) {
                    if (((i >> j) & 1) == 0)
                        continue;

                    coord_t prev_current = current;
                    current = hcs.getNeighbor(current, 2 * j + (((high_part >> j) & 1) ? 0 : 1));
                    if (hcs.IsBoundary(current)) {
                        bc_collector.push_back(current);
                        current = prev_current;
                    }
                }

                if (!bc_collector.empty()) {
                    for (auto bcc : bc_collector)
                        coeffs[bcc] += weight / bc_collector.size();
                    continue;
                }

                bool current_exists = exists(current);
                if (!current_exists || (current_exists && !isTop(current) && !use_non_top)) {
                    // we either have a non-existent coord or an existing non-top coord that we shall not use.
                    coeff_map_t partial;
                    //coeff_down_count++;
                    getCoeffs(current, partial, use_non_top, recursion + 1);
                    for (auto &coeff : partial)
                        coeffs[coeff.first] += coeff.second * weight;
                } else { // current_exists = true in this branch, so _current is valid.
                    coeffs[current] += weight;
                }
            }
        }
    }

    // .. and all levels below.
    // This routine DELETES everything in the field and is meant as an initializer.
    // If there are elements present, it throws.
    virtual void createEntireLevel(level_t level) = 0;

    // Return highest stored coord-level
    virtual level_t getHighestLevel() = 0;

    // Assignment operator requires equal structure, dirty-check with data.size()
    // isTop is not copied because of assumption of equal structure
    //virtual Field &operator=(const Field& f) = 0;

    virtual Field<DTYPE, HCSTYPE>& operator=(DTYPE f) {
        for (auto e : (*this))
            e.second = f;

        return *this;
    }

    // Arithmetic Ops, preserving structure of current refinement.
    // Exampe: a * b keeps sparse structure of a and multiplies with (possible) interpolates from b
    // while b * a keeps sparse structure of b. A generic merge() can specify merged structure and arbitrary ops.

    //virtual Field<DTYPE, HCSTYPE> operator-() const = 0;//{ Field<DTYPE, HCSTYPE> result = *this; for (auto e : result) e.second = -e.second; return result;}

    virtual Field<DTYPE, HCSTYPE>& operator*= (const Field<DTYPE, HCSTYPE>& rhs) { for (auto e : (*this)) e.second *= const_cast<Field<DTYPE, HCSTYPE>*>(&rhs)->get(e.first); return *this;}
    virtual Field<DTYPE, HCSTYPE>& operator/= (const Field<DTYPE, HCSTYPE>& rhs) { for (auto e : (*this)) e.second /= const_cast<Field<DTYPE, HCSTYPE>*>(&rhs)->get(e.first); return *this;}
    virtual Field<DTYPE, HCSTYPE>& operator+= (const Field<DTYPE, HCSTYPE>& rhs) { for (auto e : (*this)) e.second += const_cast<Field<DTYPE, HCSTYPE>*>(&rhs)->get(e.first); return *this;}
    virtual Field<DTYPE, HCSTYPE>& operator-= (const Field<DTYPE, HCSTYPE>& rhs) { for (auto e : (*this)) e.second -= const_cast<Field<DTYPE, HCSTYPE>*>(&rhs)->get(e.first); return *this;}

    virtual Field<DTYPE, HCSTYPE>& operator*= (const DTYPE& val) { for (auto e : (*this)) e.second *= val; return *this;}
    virtual Field<DTYPE, HCSTYPE>& operator/= (const DTYPE& val) { for (auto e : (*this)) e.second /= val; return *this;}
    virtual Field<DTYPE, HCSTYPE>& operator+= (const DTYPE& val) { for (auto e : (*this)) e.second += val; return *this;}
    virtual Field<DTYPE, HCSTYPE>& operator-= (const DTYPE& val) { for (auto e : (*this)) e.second -= val; return *this;}

    // Converts a Field with another DTYPE according to convert function. The structure of "this" remains.
    // The convert function must have a single argument of the foreign DTYPE2 and return DTYPE.
    // Empties "this" first.
    // Calling convention:
    //	target_field.convert< source_data_type[not Field-type!] >(source_field,
    //					[](coord_t, source_field_type) { return target_data_type;});
    // This example turns a "vector" field into a scalar field marking the
    // length of each vector:
    //  ScalarField2 vecmag;
    //  vecmag.convert<Tensor1<data_t, 2> >(v2, [](coord_t c, VectorField2 &source)->data_t {return source.get(c).length();});
    template <typename DTYPE2>
    void convert(Field<DTYPE2, HCSTYPE> &source, function<DTYPE(coord_t, Field<DTYPE2, HCSTYPE> &)> convert_fn) {

        for (auto it = begin(true); it != end(); ++it) {
            coord_t own_coord = (*it).first;
            (*it).second = convert_fn(own_coord, source);
        }

    }

    // Merge 2 fields with possible foreign data type into "this".
    // Arbitrary operations possible through the converter function.
    // The structure of "this" remains.
    // merger function must have 2 arguments of foreign DTYPE2& and return DTYPE.
    // The resulting structure will be the one of f1!
    template <typename DTYPE2>
    void merge(Field<DTYPE2, HCSTYPE> &source1, Field<DTYPE2, HCSTYPE> &source2, function<DTYPE(coord_t, DTYPE2, DTYPE2)> merge_fn) {

        for (auto it = begin(true); it != end(); ++it) {
            coord_t own_coord = (*it).first;
            (*it).second = merge_fn(own_coord, source1.get(own_coord), source2.get(own_coord));
        }
    }

    // Empties all data
    virtual void clear() = 0;


};

// Other non-member arithmetic ops
// serve as template for overriding, result and LHS must be of inherited class, RHS can stay generic field as its iterator / copy constructpr is not needed
/*
template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator* (const Field<DTYPE, HCSTYPE>& lhs, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result *= rhs;
	return result;
};

template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator* (const DTYPE& val, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = rhs;
	result *= val;
	return result;
};

template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator* (const Field<DTYPE, HCSTYPE>& lhs, const DTYPE& val) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result *= val;
	return result;
};

template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator/ (const Field<DTYPE, HCSTYPE>& lhs, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result /= rhs;
	return result;
};
template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator/ (const DTYPE& val, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = rhs;
	for (auto e : result)
		e.second = val / e.second;
	return result;
}
template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator/ (const Field<DTYPE, HCSTYPE>& lhs, const DTYPE& val) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result /= val;
	return result;
}

template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator+ (const Field<DTYPE, HCSTYPE>& lhs, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result += rhs;
	return result;
};
template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator+ (const DTYPE& val, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = rhs;
	result += val;
	return result;
}
template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator+ (const Field<DTYPE, HCSTYPE>& lhs, const DTYPE& val) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result += val;
	return result;
}

template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator- (const Field<DTYPE, HCSTYPE>& lhs, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result -= rhs;
	return result;
};
template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator- (const DTYPE& val, const Field<DTYPE, HCSTYPE>& rhs) {
	Field<DTYPE, HCSTYPE> result = -rhs;
	result += val;
	return result;
}
template <typename DTYPE, typename HCSTYPE> Field<DTYPE, HCSTYPE> operator- (const Field<DTYPE, HCSTYPE>& lhs, const DTYPE& val) {
	Field<DTYPE, HCSTYPE> result = lhs;
	result -= val;
	return result;
}
 */

