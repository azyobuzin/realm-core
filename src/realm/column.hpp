/*************************************************************************
 *
 * REALM CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2012] Realm Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of Realm Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to Realm Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from Realm Incorporated.
 *
 **************************************************************************/
#ifndef REALM_COLUMN_HPP
#define REALM_COLUMN_HPP

#include <stdint.h> // unint8_t etc
#include <cstdlib> // std::size_t
#include <vector>
#include <memory>

#include <realm/array_integer.hpp>
#include <realm/column_type.hpp>
#include <realm/column_fwd.hpp>
#include <realm/spec.hpp>
#include <realm/impl/output_stream.hpp>
#include <realm/query_conditions.hpp>
#include <realm/bptree.hpp>
#include <realm/index_string.hpp>
#include <realm/impl/destroy_guard.hpp>

namespace realm {


// Pre-definitions
class StringIndex;

struct ColumnTemplateBase
{
    virtual int compare_values(size_t row1, size_t row2) const = 0;
};

template <class T> struct ColumnTemplate : public ColumnTemplateBase
{
    // Overridden in column_string.* because == operator of StringData isn't yet locale aware; todo
    virtual int compare_values(size_t row1, size_t row2) const
    {
        T a = get_val(row1);
        T b = get_val(row2);
        return a == b ? 0 : a < b ? 1 : -1;
    }

    // We cannot use already-existing get() methods because ColumnStringEnum and LinkList inherit from
    // Column and overload get() with different return type than int64_t. Todo, find a way to simplify
    virtual T get_val(size_t row) const = 0;
};

/// Base class for all column types.
class ColumnBase {
public:
    /// Get the number of entries in this column. This operation is relatively
    /// slow.
    virtual std::size_t size() const REALM_NOEXCEPT = 0;

    /// \throw LogicError Thrown if this column is not string valued.
    virtual void set_string(std::size_t row_ndx, StringData value);

    /// Insert the specified number of default values into this column starting
    /// at the specified row index. Set `is_append` to true if, and only if
    /// `row_ndx` is equal to the size of the column (before insertion).
    virtual void insert(std::size_t row_ndx, std::size_t num_rows, bool is_append) = 0;

    /// Remove all elements from this column.
    ///
    /// \param num_rows The total number of rows in this column.
    ///
    /// \param broken_reciprocal_backlinks If true, link columns must assume
    /// that reciprocal backlinks have already been removed. Non-link columns,
    /// and backlink columns should ignore this argument.
    virtual void clear(std::size_t num_rows, bool broken_reciprocal_backlinks) = 0;

    /// Remove the specified entry from this column. Set \a is_last to
    /// true when deleting the last element. This is important to
    /// avoid conversion to to general form of inner nodes of the
    /// B+-tree.
    virtual void erase(std::size_t row_ndx, bool is_last) = 0;

    /// Remove the specified row by moving the last row over it. This reduces the
    /// number of elements by one. The specified last row index must always be
    /// one less than the number of rows in the column.
    ///
    /// \param broken_reciprocal_backlinks If true, link columns must assume
    /// that reciprocal backlinks have already been removed for the specified
    /// row. Non-link columns, and backlink columns should ignore this argument.
    virtual void move_last_over(std::size_t row_ndx, std::size_t last_row_ndx,
                                bool broken_reciprocal_backlinks) = 0;

    virtual bool IsIntColumn() const REALM_NOEXCEPT { return false; }

    // Returns true if, and only if this column is an AdaptiveStringColumn.
    virtual bool is_string_col() const REALM_NOEXCEPT;

    virtual void destroy() REALM_NOEXCEPT = 0;
    void move_assign(ColumnBase& col) REALM_NOEXCEPT;

    virtual ~ColumnBase() REALM_NOEXCEPT {}

    // Getter function for index. For integer index, the caller must supply a buffer that we can store the
    // extracted value in (it may be bitpacked, so we cannot return a pointer in to the Array as we do with
    // String index).
    virtual StringData get_index_data(std::size_t, char*) const REALM_NOEXCEPT = 0;

    // Search index
    virtual bool has_search_index() const REALM_NOEXCEPT;
    virtual StringIndex* create_search_index();
    virtual void destroy_search_index() REALM_NOEXCEPT;
    virtual const StringIndex* get_search_index() const REALM_NOEXCEPT;
    virtual StringIndex* get_search_index() REALM_NOEXCEPT;
    virtual void set_search_index_ref(ref_type, ArrayParent*, std::size_t ndx_in_parent,
                                      bool allow_duplicate_values);
    virtual void set_search_index_allow_duplicate_values(bool) REALM_NOEXCEPT;

    virtual Allocator& get_alloc() const REALM_NOEXCEPT = 0;

    /// Returns the 'ref' of the root array.
    virtual ref_type get_ref() const REALM_NOEXCEPT = 0;

    virtual void replace_root_array(std::unique_ptr<Array> leaf) = 0;
    virtual MemRef clone_deep(Allocator& alloc) const = 0;
    virtual void detach(void) = 0;
    virtual bool is_attached(void) const REALM_NOEXCEPT = 0;

    static std::size_t get_size_from_type_and_ref(ColumnType, ref_type, Allocator&) REALM_NOEXCEPT;

    // These assume that the right column compile-time type has been
    // figured out.
    static std::size_t get_size_from_ref(ref_type root_ref, Allocator&);
    static std::size_t get_size_from_ref(ref_type spec_ref, ref_type columns_ref, Allocator&);

    /// Write a slice of this column to the specified output stream.
    virtual ref_type write(std::size_t slice_offset, std::size_t slice_size,
                           std::size_t table_size, _impl::OutputStream&) const = 0;

    virtual void set_parent(ArrayParent*, std::size_t ndx_in_parent) REALM_NOEXCEPT = 0;
    virtual std::size_t get_ndx_in_parent() const REALM_NOEXCEPT = 0;
    virtual void set_ndx_in_parent(std::size_t ndx_in_parent) REALM_NOEXCEPT = 0;

    /// Called in the context of Group::commit() and
    /// SharedGroup::commit_and_continue_as_read()() to ensure that attached
    /// table and link list accessors stay valid across a commit.
    virtual void update_from_parent(std::size_t old_baseline) REALM_NOEXCEPT = 0;

    //@{

    /// cascade_break_backlinks_to() is called iteratively for each column by
    /// Table::cascade_break_backlinks_to() with the same arguments as are
    /// passed to Table::cascade_break_backlinks_to(). Link columns must
    /// override it. The same is true for cascade_break_backlinks_to_all_rows(),
    /// except that it is called from
    /// Table::cascade_break_backlinks_to_all_rows(), and that it expects
    /// Table::cascade_break_backlinks_to_all_rows() to pass the number of rows
    /// in the table as \a num_rows.

    struct CascadeState;
    virtual void cascade_break_backlinks_to(std::size_t row_ndx, CascadeState&);
    virtual void cascade_break_backlinks_to_all_rows(std::size_t num_rows, CascadeState&);

    //@}

    void discard_child_accessors() REALM_NOEXCEPT;

    /// For columns that are able to contain subtables, this function returns
    /// the pointer to the subtable accessor at the specified row index if it
    /// exists, otherwise it returns null. For other column types, this function
    /// returns null.
    virtual Table* get_subtable_accessor(std::size_t row_ndx) const REALM_NOEXCEPT;

    /// Detach and remove the subtable accessor at the specified row if it
    /// exists. For column types that are unable to contain subtable, this
    /// function does nothing.
    virtual void discard_subtable_accessor(std::size_t row_ndx) REALM_NOEXCEPT;

    virtual void adj_acc_insert_rows(std::size_t row_ndx, std::size_t num_rows) REALM_NOEXCEPT;
    virtual void adj_acc_erase_row(std::size_t row_ndx) REALM_NOEXCEPT;
    /// See Table::adj_acc_move_over()
    virtual void adj_acc_move_over(std::size_t from_row_ndx,
                                   std::size_t to_row_ndx) REALM_NOEXCEPT;
    virtual void adj_acc_clear_root_table() REALM_NOEXCEPT;

    enum {
        mark_Recursive   = 0x01,
        mark_LinkTargets = 0x02,
        mark_LinkOrigins = 0x04
    };

    virtual void mark(int type) REALM_NOEXCEPT;

    virtual void bump_link_origin_table_version() REALM_NOEXCEPT;

    /// Refresh the dirty part of the accessor subtree rooted at this column
    /// accessor.
    ///
    /// The following conditions are necessary and sufficient for the proper
    /// operation of this function:
    ///
    ///  - The parent table accessor (excluding its column accessors) is in a
    ///    valid state (already refreshed).
    ///
    ///  - Every subtable accessor in the subtree is marked dirty if it needs to
    ///    be refreshed, or if it has a descendant accessor that needs to be
    ///    refreshed.
    ///
    ///  - This column accessor, as well as all its descendant accessors, are in
    ///    structural correspondence with the underlying node hierarchy whose
    ///    root ref is stored in the parent (`Table::m_columns`) (see
    ///    AccessorConsistencyLevels).
    ///
    ///  - The 'index in parent' property of the cached root array
    ///    (`root->m_ndx_in_parent`) is valid.
    virtual void refresh_accessor_tree(std::size_t new_col_ndx, const Spec&) = 0;

#ifdef REALM_DEBUG
    // Must be upper case to avoid conflict with macro in Objective-C
    virtual void Verify() const = 0;
    virtual void Verify(const Table&, std::size_t col_ndx) const;
    virtual void to_dot(std::ostream&, StringData title = StringData()) const = 0;
    void dump_node_structure() const; // To std::cerr (for GDB)
    virtual void do_dump_node_structure(std::ostream&, int level) const = 0;
    void bptree_to_dot(const Array* root, std::ostream& out) const;
#endif

protected:
    using SliceHandler = BpTreeBase::SliceHandler;

    ColumnBase() {}
    ColumnBase(ColumnBase&&) = default;

    // Must not assume more than minimal consistency (see
    // AccessorConsistencyLevels).
    virtual void do_discard_child_accessors() REALM_NOEXCEPT {}

    //@{
    /// \tparam L Any type with an appropriate `value_type`, %size(),
    /// and %get() members.
    template<class L, class T>
    std::size_t lower_bound(const L& list, T value) const REALM_NOEXCEPT;
    template<class L, class T>
    std::size_t upper_bound(const L& list, T value) const REALM_NOEXCEPT;
    //@}

    // Node functions
    template <class T, class R, Action action, class condition>
    R aggregate(T target, std::size_t start, std::size_t end, size_t limit = size_t(-1),
                size_t* return_ndx = nullptr) const;

    class CreateHandler {
    public:
        virtual ref_type create_leaf(std::size_t size) = 0;
        ~CreateHandler() REALM_NOEXCEPT {}
    };

    static ref_type create(Allocator&, std::size_t size, CreateHandler&);

#ifdef REALM_DEBUG
    class LeafToDot;
    virtual void leaf_to_dot(MemRef, ArrayParent*, std::size_t ndx_in_parent,
                             std::ostream&) const = 0;
#endif

private:
    class WriteSliceHandler;

    static ref_type build(std::size_t* rest_size_ptr, std::size_t fixed_height,
                          Allocator&, CreateHandler&);
};


// FIXME: Temporary class until all column types have been migrated to use BpTree interface
class ColumnBaseSimple : public ColumnBase {
public:
    //@{
    /// Returns the array node at the root of this column, but note
    /// that there is no guarantee that this node is an inner B+-tree
    /// node or a leaf. This is the case for a MixedColumn in
    /// particular.
    Array* get_root_array() REALM_NOEXCEPT { return m_array.get(); }
    const Array* get_root_array() const REALM_NOEXCEPT { return m_array.get(); }
    //@}

    Allocator& get_alloc() const REALM_NOEXCEPT final { return m_array->get_alloc(); }
    void destroy() REALM_NOEXCEPT override { if (m_array) m_array->destroy_deep(); }
    ref_type get_ref() const REALM_NOEXCEPT final { return m_array->get_ref(); }
    void detach() REALM_NOEXCEPT final { m_array->detach(); }
    bool is_attached() const REALM_NOEXCEPT final { return m_array->is_attached(); }
    void set_parent(ArrayParent* parent, std::size_t ndx_in_parent) REALM_NOEXCEPT final { m_array->set_parent(parent, ndx_in_parent); }
    std::size_t get_ndx_in_parent() const REALM_NOEXCEPT final { return m_array->get_ndx_in_parent(); }
    void set_ndx_in_parent(std::size_t ndx_in_parent) REALM_NOEXCEPT final { m_array->set_ndx_in_parent(ndx_in_parent); }
    void update_from_parent(std::size_t old_baseline) REALM_NOEXCEPT override { m_array->update_from_parent(old_baseline); }
    MemRef clone_deep(Allocator& alloc) const override { return m_array->clone_deep(alloc); }
protected:
    ColumnBaseSimple() {}
    ColumnBaseSimple(Array* root) : m_array(root) {}
    std::unique_ptr<Array> m_array;

    void replace_root_array(std::unique_ptr<Array> new_root) final;
    bool root_is_leaf() const REALM_NOEXCEPT { return !m_array->is_inner_bptree_node(); }

    /// Introduce a new root node which increments the height of the
    /// tree by one.
    void introduce_new_root(ref_type new_sibling_ref, Array::TreeInsertBase& state,
                            bool is_append);

    static ref_type write(const Array* root, std::size_t slice_offset, std::size_t slice_size,
                          std::size_t table_size, SliceHandler&, _impl::OutputStream&);

#if defined(REALM_DEBUG)
    void tree_to_dot(std::ostream&) const;
#endif
};

class ColumnBaseWithIndex : public ColumnBase {
public:
    ~ColumnBaseWithIndex() REALM_NOEXCEPT override {}
    void set_ndx_in_parent(std::size_t ndx) REALM_NOEXCEPT override;
    void update_from_parent(std::size_t old_baseline) REALM_NOEXCEPT override;
    void refresh_accessor_tree(std::size_t, const Spec&) override;
    void move_assign(ColumnBaseWithIndex& col) REALM_NOEXCEPT;
    void destroy() REALM_NOEXCEPT override;

    bool has_search_index() const REALM_NOEXCEPT final { return bool(m_search_index); }
    StringIndex* get_search_index() REALM_NOEXCEPT final { return m_search_index.get(); }
    const StringIndex* get_search_index() const REALM_NOEXCEPT final { return m_search_index.get(); }
    void destroy_search_index() REALM_NOEXCEPT override;
    void set_search_index_ref(ref_type ref, ArrayParent* parent,
            size_t ndx_in_parent, bool allow_duplicate_valaues) final;
    StringIndex* create_search_index() override = 0;
protected:
    ColumnBaseWithIndex() {}
    ColumnBaseWithIndex(ColumnBaseWithIndex&&) = default;
    std::unique_ptr<StringIndex> m_search_index;
};


struct ColumnBase::CascadeState {
    struct row {
        std::size_t table_ndx; ///< Index within group of a group-level table.
        std::size_t row_ndx;

        bool operator==(const row&) const REALM_NOEXCEPT;
        bool operator!=(const row&) const REALM_NOEXCEPT;

        /// Trivial lexicographic order
        bool operator<(const row&) const REALM_NOEXCEPT;
    };

    typedef std::vector<row> row_set;

    /// A sorted list of rows. The order is defined by row::operator<(), and
    /// insertions must respect this order.
    row_set rows;

    /// If non-null, then no recursion will be performed for rows of that
    /// table. The effect is then exactly as if all the rows of that table were
    /// added to \a state.rows initially, and then removed again after the
    /// explicit invocations of Table::cascade_break_backlinks_to() (one for
    /// each initiating row). This is used by Table::clear() to avoid
    /// reentrance.
    ///
    /// Must never be set concurrently with stop_on_link_list_column.
    Table* stop_on_table;

    /// If non-null, then Table::cascade_break_backlinks_to() will skip the
    /// removal of reciprocal backlinks for the link list at
    /// stop_on_link_list_row_ndx in this column, and no recursion will happen
    /// on its behalf. This is used by LinkView::clear() to avoid reentrance.
    ///
    /// Must never be set concurrently with stop_on_table.
    ColumnLinkList* stop_on_link_list_column;

    /// Is ignored if stop_on_link_list_column is null.
    std::size_t stop_on_link_list_row_ndx;

    CascadeState();
};


/// A column (TColumn) is a single B+-tree, and the root of
/// the column is the root of the B+-tree. All leaf nodes are arrays.
///
// FIXME: Rename TColumn to Column when Column has been renamed to IntegerColumn.
template <class T, bool Nullable>
class TColumn : public ColumnBaseWithIndex, public ColumnTemplate<T> {
public:
    using value_type = T;
    using LeafInfo = typename BpTree<T, Nullable>::LeafInfo;

    struct unattached_root_tag {};

    explicit TColumn() REALM_NOEXCEPT : m_tree(Allocator::get_default()) {}
    explicit TColumn(std::unique_ptr<Array> root) REALM_NOEXCEPT;
    TColumn(Allocator&, ref_type);
    TColumn(unattached_root_tag, Allocator&);
    TColumn(TColumn<T, Nullable>&&) REALM_NOEXCEPT = default;
    ~TColumn() REALM_NOEXCEPT override;
	
	void init_from_parent();

    // Accessor concept:
    void destroy() REALM_NOEXCEPT override;
    Allocator& get_alloc() const REALM_NOEXCEPT final;
    ref_type get_ref() const REALM_NOEXCEPT final;
    void set_parent(ArrayParent* parent, std::size_t ndx_in_parent) REALM_NOEXCEPT override;
    std::size_t get_ndx_in_parent() const REALM_NOEXCEPT final;
    void set_ndx_in_parent(std::size_t ndx) REALM_NOEXCEPT final;
    void update_from_parent(std::size_t old_baseline) REALM_NOEXCEPT override;
    void refresh_accessor_tree(std::size_t, const Spec&) override;
    void detach() REALM_NOEXCEPT final;
    bool is_attached() const REALM_NOEXCEPT final;
    MemRef clone_deep(Allocator&) const override;

    void move_assign(TColumn<T, Nullable>&);
    bool IsIntColumn() const REALM_NOEXCEPT override;

    std::size_t size() const REALM_NOEXCEPT override;
    bool is_empty() const REALM_NOEXCEPT { return size() == 0; }

    /// Provides access to the leaf that contains the element at the
    /// specified index. Upon return \a ndx_in_leaf will be set to the
    /// corresponding index relative to the beginning of the leaf.
    ///
    /// LeafInfo is a struct defined by the underlying BpTree<T,N>
    /// data structure, that provides a way for the caller to do
    /// leaf caching without instantiating too many objects along
    /// the way.
    ///
    /// This function cannot be used for modifying operations as it
    /// does not ensure the presence of an unbroken chain of parent
    /// accessors. For this reason, the identified leaf should always
    /// be accessed through the returned const-qualified reference,
    /// and never directly through the specfied fallback accessor.
    void get_leaf(std::size_t ndx, std::size_t& ndx_in_leaf,
        LeafInfo& inout_leaf) const REALM_NOEXCEPT;

    // Getting and setting values
    T get_val(std::size_t ndx) const REALM_NOEXCEPT final { return get(ndx); }
    T get(std::size_t ndx) const REALM_NOEXCEPT;
    T back() const REALM_NOEXCEPT;
    void set(std::size_t, T value);
    void add(T value = T{});
    void insert(std::size_t ndx, T value = T{}, std::size_t num_rows = 1);
    void erase(std::size_t ndx);
    void move_last_over(std::size_t row_ndx, std::size_t last_row_ndx);
    void clear();

    // Index support
    StringData get_index_data(std::size_t ndx, char* buffer) const REALM_NOEXCEPT override;

    // FIXME: Remove these
    uint64_t get_uint(std::size_t ndx) const REALM_NOEXCEPT;
    ref_type get_as_ref(std::size_t ndx) const REALM_NOEXCEPT;
    void set_uint(std::size_t ndx, uint64_t value);
    void set_as_ref(std::size_t ndx, ref_type value);

    void destroy_subtree(std::size_t ndx, bool clear_value);

    template <class U>
    void adjust(std::size_t ndx, U diff);
    template <class U>
    void adjust(U diff);
    template <class U>
    void adjust_ge(T limit, U diff);

    std::size_t count(T target) const;

    T sum(std::size_t start = 0, std::size_t end = npos, std::size_t limit = npos,
                std::size_t* return_ndx = nullptr) const;

    T maximum(std::size_t start = 0, std::size_t end = npos, std::size_t limit = npos,
                    std::size_t* return_ndx = nullptr) const;

    T minimum(std::size_t start = 0, std::size_t end = npos, std::size_t limit = npos,
                    std::size_t* return_ndx = nullptr) const;

    double average(std::size_t start = 0, std::size_t end = npos, std::size_t limit = npos,
                    std::size_t* return_ndx = nullptr) const;

    std::size_t find_first(T value, std::size_t begin = 0, std::size_t end = npos) const;
    void find_all(TColumn<int64_t, false>& out_indices, T value,
                  std::size_t begin = 0, std::size_t end = npos) const;

    void populate_search_index();
    StringIndex* create_search_index() override;

    //@{
    /// Find the lower/upper bound for the specified value assuming
    /// that the elements are already sorted in ascending order
    /// according to ordinary integer comparison.
    // FIXME: Rename
    std::size_t lower_bound_int(T value) const REALM_NOEXCEPT;
    // FIXME: Rename
    std::size_t upper_bound_int(T value) const REALM_NOEXCEPT;
    //@}

    std::size_t find_gte(T target, std::size_t start) const;

    // FIXME: Rename
    bool compare_int(const TColumn<T, Nullable>&) const REALM_NOEXCEPT;

    static ref_type create(Allocator&, Array::Type leaf_type = Array::type_Normal,
                           std::size_t size = 0, T value = 0);

    // Overriding method in ColumnBase
    ref_type write(std::size_t, std::size_t, std::size_t,
                   _impl::OutputStream&) const override;

    void insert(std::size_t, std::size_t, bool) override;
    void erase(std::size_t, bool) override;
    void move_last_over(std::size_t, std::size_t, bool) override;
    void clear(std::size_t, bool) override;

    /// \param row_ndx Must be `realm::npos` if appending.
    void insert_without_updating_index(std::size_t row_ndx, T value, std::size_t num_rows);

#ifdef REALM_DEBUG
    void Verify() const override;
    using ColumnBase::Verify;
    void to_dot(std::ostream&, StringData title) const override;
    void tree_to_dot(std::ostream&) const;
    MemStats stats() const;
    void do_dump_node_structure(std::ostream&, int) const override;
#endif

    //@{
    /// Returns the array node at the root of this column, but note
    /// that there is no guarantee that this node is an inner B+-tree
    /// node or a leaf. This is the case for a MixedColumn in
    /// particular.
    Array* get_root_array() REALM_NOEXCEPT { return &m_tree.root(); }
    const Array* get_root_array() const REALM_NOEXCEPT { return &m_tree.root(); }
    //@}

protected:
    bool root_is_leaf() const REALM_NOEXCEPT { return m_tree.root_is_leaf(); }
    void replace_root_array(std::unique_ptr<Array> leaf) final { m_tree.replace_root(std::move(leaf)); }

    void set_without_updating_index(std::size_t row_ndx, T value);
    void erase_without_updating_index(std::size_t row_ndx, bool is_last);
    void move_last_over_without_updating_index(std::size_t row_ndx, std::size_t last_row_ndx);

    /// If any element points to an array node, this function recursively
    /// destroys that array node. Note that the same is **not** true for
    /// Column::do_erase() and Column::do_move_last_over().
    ///
    /// FIXME: Be careful, clear_without_updating_index() currently forgets
    /// if the leaf type is Array::type_HasRefs.
    void clear_without_updating_index();

#ifdef REALM_DEBUG
    void leaf_to_dot(MemRef, ArrayParent*, std::size_t ndx_in_parent,
                     std::ostream&) const override;
    static void dump_node_structure(const Array& root, std::ostream&, int level);
#endif

private:
    class EraseLeafElem;
    class CreateHandler;
    class SliceHandler;

    friend class Array;
    friend class ColumnBase;
    friend class StringIndex;

    BpTree<T, Nullable> m_tree;
};

template <> struct GetLeafType<int64_t, false> {
    using type = ArrayInteger;
};


// Implementation:

inline bool ColumnBase::is_string_col() const REALM_NOEXCEPT
{
    return false;
}

inline bool ColumnBase::has_search_index() const REALM_NOEXCEPT
{
    return get_search_index() != nullptr;
}

inline StringIndex* ColumnBase::create_search_index()
{
    return nullptr;
}

inline void ColumnBase::destroy_search_index() REALM_NOEXCEPT
{
}

inline const StringIndex* ColumnBase::get_search_index() const REALM_NOEXCEPT
{
    return nullptr;
}

inline StringIndex* ColumnBase::get_search_index() REALM_NOEXCEPT
{
    return nullptr;
}

inline void ColumnBase::set_search_index_ref(ref_type, ArrayParent*, std::size_t, bool)
{
}

inline void ColumnBase::set_search_index_allow_duplicate_values(bool) REALM_NOEXCEPT
{
}

inline bool ColumnBase::CascadeState::row::operator==(const row& r) const REALM_NOEXCEPT
{
    return table_ndx == r.table_ndx && row_ndx == r.row_ndx;
}

inline bool ColumnBase::CascadeState::row::operator!=(const row& r) const REALM_NOEXCEPT
{
    return !(*this == r);
}

inline bool ColumnBase::CascadeState::row::operator<(const row& r) const REALM_NOEXCEPT
{
    return table_ndx < r.table_ndx || (table_ndx == r.table_ndx && row_ndx < r.row_ndx);
}

inline ColumnBase::CascadeState::CascadeState():
    stop_on_table(0),
    stop_on_link_list_column(0),
    stop_on_link_list_row_ndx(0)
{
}

inline void ColumnBase::discard_child_accessors() REALM_NOEXCEPT
{
    do_discard_child_accessors();
}

inline Table* ColumnBase::get_subtable_accessor(std::size_t) const REALM_NOEXCEPT
{
    return 0;
}

inline void ColumnBase::discard_subtable_accessor(std::size_t) REALM_NOEXCEPT
{
    // Noop
}

inline void ColumnBase::adj_acc_insert_rows(std::size_t, std::size_t) REALM_NOEXCEPT
{
    // Noop
}

inline void ColumnBase::adj_acc_erase_row(std::size_t) REALM_NOEXCEPT
{
    // Noop
}

inline void ColumnBase::adj_acc_move_over(std::size_t, std::size_t) REALM_NOEXCEPT
{
    // Noop
}

inline void ColumnBase::adj_acc_clear_root_table() REALM_NOEXCEPT
{
    // Noop
}

inline void ColumnBase::mark(int) REALM_NOEXCEPT
{
    // Noop
}

inline void ColumnBase::bump_link_origin_table_version() REALM_NOEXCEPT
{
    // Noop
}

template <class T, bool N>
void TColumn<T, N>::set_without_updating_index(std::size_t ndx, T value)
{
    m_tree.set(ndx, std::move(value));
}

template <class T, bool N>
void TColumn<T, N>::set(std::size_t ndx, T value)
{
    REALM_ASSERT_DEBUG(ndx < size());
    if (has_search_index()) {
        m_search_index->set(ndx, value);
    }
    set_without_updating_index(ndx, std::move(value));
}

// When a value of a signed type is converted to an unsigned type, the C++ standard guarantees that negative values
// are converted from the native representation to 2's complement, but the opposite conversion is left as undefined.
// realm::util::from_twos_compl() is used here to perform the correct opposite unsigned-to-signed conversion,
// which reduces to a no-op when 2's complement is the native representation of negative values.
template <class T, bool N>
void TColumn<T, N>::set_uint(std::size_t ndx, uint64_t value)
{
    set(ndx, util::from_twos_compl<int_fast64_t>(value));
}

template <class T, bool N>
void TColumn<T, N>::set_as_ref(std::size_t ndx, ref_type ref)
{
    set(ndx, from_ref(ref));
}

template <class T, bool N>
template <class U>
void TColumn<T, N>::adjust(std::size_t ndx, U diff)
{
    REALM_ASSERT_3(ndx, <, size());
    m_tree.adjust(ndx, diff);
}

template <class T, bool N>
template <class U>
void TColumn<T, N>::adjust(U diff)
{
    m_tree.adjust(diff);
}

template <class T, bool N>
template <class U>
void TColumn<T, N>::adjust_ge(T limit, U diff)
{
    m_tree.adjust_ge(limit, diff);
}

template <class T, bool N>
std::size_t TColumn<T, N>::count(T target) const
{
    if (has_search_index()) {
        return m_search_index->count(target);
    }
    return aggregate<T, T, act_Count, Equal>(target, 0, size());
}

template <class T, bool N>
T TColumn<T, N>::sum(std::size_t start, std::size_t end, std::size_t limit, std::size_t* return_ndx) const
{
    return aggregate<T, T, act_Sum, None>(0, start, end, limit, return_ndx);
}

template <class T, bool N>
double TColumn<T, N>::average(std::size_t start, std::size_t end, std::size_t limit, std::size_t* return_ndx) const
{
    if (end == size_t(-1))
        end = size();
    size_t size = end - start;
    if(limit < size)
        size = limit;
    auto s = sum(start, end, limit, return_ndx);
    double avg = double(s) / double(size == 0 ? 1 : size);
    return avg;
}

template <class T, bool N>
T TColumn<T,N>::minimum(size_t start, size_t end, size_t limit, size_t* return_ndx) const
{
    return aggregate<T, T, act_Min, None>(0, start, end, limit, return_ndx);
}

template <class T, bool N>
T TColumn<T,N>::maximum(size_t start, size_t end, size_t limit, size_t* return_ndx) const
{
    return aggregate<T, T, act_Max, None>(0, start, end, limit, return_ndx);
}

template <class T, bool N>
void TColumn<T,N>::destroy_subtree(size_t ndx, bool clear_value)
{
    static_assert(std::is_same<T, int_fast64_t>::value && !N,
        "destroy_subtree only makes sense on non-nullable integer columns");
    int_fast64_t value = get(ndx);

    // Null-refs indicate empty subtrees
    if (value == 0)
        return;

    // A ref is always 8-byte aligned, so the lowest bit
    // cannot be set. If it is, it means that it should not be
    // interpreted as a ref.
    if (value % 2 != 0)
        return;

    // Delete subtree
    ref_type ref = to_ref(value);
    Allocator& alloc = get_alloc();
    Array::destroy_deep(ref, alloc);

    if (clear_value)
        set(ndx, 0); // Throws
}

template <class T, bool N>
void TColumn<T, N>::get_leaf(std::size_t ndx, std::size_t& ndx_in_leaf,
                             typename BpTree<T,N>::LeafInfo& inout_leaf_info) const REALM_NOEXCEPT
{
    m_tree.get_leaf(ndx, ndx_in_leaf, inout_leaf_info);
}

template <class T, bool N>
StringData TColumn<T, N>::get_index_data(std::size_t ndx, char* buffer) const REALM_NOEXCEPT
{
    static_assert(sizeof(T) == 8, "not filling buffer");
    T x = get(ndx);
    *reinterpret_cast<T*>(buffer) = x;
	  return StringData(buffer, sizeof(T));
}

template <class T, bool N>
void TColumn<T,N>::populate_search_index()
{
    REALM_ASSERT(has_search_index());
    // Populate the index
    std::size_t num_rows = size();
    for (std::size_t row_ndx = 0; row_ndx != num_rows; ++row_ndx) {
        T value = get(row_ndx);
        size_t num_rows = 1;
        bool is_append = true;
        m_search_index->insert(row_ndx, value, num_rows, is_append); // Throws
    }
}

template <class T, bool N>
StringIndex* TColumn<T, N>::create_search_index()
{
    REALM_ASSERT(!has_search_index());
    m_search_index.reset(new StringIndex(this, get_alloc())); // Throws
    populate_search_index();
    return m_search_index.get();
}

template <class T, bool N>
std::size_t TColumn<T,N>::find_first(T value, std::size_t begin, std::size_t end) const
{
    REALM_ASSERT_3(begin, <=, size());
    REALM_ASSERT(end == npos || (begin <= end && end <= size()));

    if (m_search_index && begin == 0 && end == npos)
        return m_search_index->find_first(value);
    return m_tree.find_first(value, begin, end);
}

template <class T, bool N>
void TColumn<T,N>::find_all(Column& result, T value, size_t begin, size_t end) const
{
    REALM_ASSERT_3(begin, <=, size());
    REALM_ASSERT(end == npos || (begin <= end && end <= size()));

    if (m_search_index && begin == 0 && end == npos)
        return m_search_index->find_all(result, value);
    return m_tree.find_all(result, value, begin, end);
}

inline std::size_t ColumnBase::get_size_from_ref(ref_type root_ref, Allocator& alloc)
{
    const char* root_header = alloc.translate(root_ref);
    bool root_is_leaf = !Array::get_is_inner_bptree_node_from_header(root_header);
    if (root_is_leaf)
        return Array::get_size_from_header(root_header);
    return Array::get_bptree_size_from_header(root_header);
}

template<class L, class T>
std::size_t ColumnBase::lower_bound(const L& list, T value) const REALM_NOEXCEPT
{
    std::size_t i = 0;
    std::size_t size = list.size();
    while (0 < size) {
        std::size_t half = size / 2;
        std::size_t mid = i + half;
        typename L::value_type probe = list.get(mid);
        if (probe < value) {
            i = mid + 1;
            size -= half + 1;
        }
        else {
            size = half;
        }
    }
    return i;
}

template<class L, class T>
std::size_t ColumnBase::upper_bound(const L& list, T value) const REALM_NOEXCEPT
{
    size_t i = 0;
    size_t size = list.size();
    while (0 < size) {
        size_t half = size / 2;
        size_t mid = i + half;
        typename L::value_type probe = list.get(mid);
        if (!(value < probe)) {
            i = mid + 1;
            size -= half + 1;
        }
        else {
            size = half;
        }
    }
    return i;
}


inline ref_type ColumnBase::create(Allocator& alloc, std::size_t size, CreateHandler& handler)
{
    std::size_t rest_size = size;
    std::size_t fixed_height = 0; // Not fixed
    return build(&rest_size, fixed_height, alloc, handler);
}

template <class T, bool N>
TColumn<T,N>::TColumn(Allocator& alloc, ref_type ref) : m_tree(BpTreeBase::unattached_tag{})
{
    // fixme, must m_search_index be copied here?
    m_tree.init_from_ref(alloc, ref);
}

template <class T, bool N>
TColumn<T,N>::TColumn(unattached_root_tag, Allocator& alloc) : m_tree(alloc)
{
}

template <class T, bool N>
TColumn<T,N>::TColumn(std::unique_ptr<Array> root) REALM_NOEXCEPT : m_tree(std::move(root))
{
}

template <class T, bool N>
TColumn<T,N>::~TColumn() REALM_NOEXCEPT
{
}

template <class T, bool N>
void TColumn<T,N>::init_from_parent()
{
	m_tree.init_from_parent();
}

template <class T, bool N>
void TColumn<T,N>::destroy() REALM_NOEXCEPT
{
    ColumnBaseWithIndex::destroy();
    m_tree.destroy();
}

template <class T, bool N>
void TColumn<T,N>::move_assign(TColumn<T,N>& col)
{
    ColumnBaseWithIndex::move_assign(col);
    m_tree = std::move(col.m_tree);
}

template <class T, bool N>
bool TColumn<T,N>::IsIntColumn() const REALM_NOEXCEPT
{
    return std::is_integral<T>::value;
}

template <class T, bool N>
Allocator& TColumn<T,N>::get_alloc() const REALM_NOEXCEPT
{
    return m_tree.get_alloc();
}

template <class T, bool N>
void TColumn<T,N>::set_parent(ArrayParent* parent, std::size_t ndx_in_parent) REALM_NOEXCEPT
{
    m_tree.set_parent(parent, ndx_in_parent);
}

template <class T, bool N>
std::size_t TColumn<T,N>::get_ndx_in_parent() const REALM_NOEXCEPT
{
    return m_tree.get_ndx_in_parent();
}

template <class T, bool N>
void TColumn<T,N>::set_ndx_in_parent(std::size_t ndx_in_parent) REALM_NOEXCEPT
{
    ColumnBaseWithIndex::set_ndx_in_parent(ndx_in_parent);
    m_tree.set_ndx_in_parent(ndx_in_parent);
}

template <class T, bool N>
void TColumn<T,N>::detach() REALM_NOEXCEPT
{
    m_tree.detach();
}

template <class T, bool N>
bool TColumn<T,N>::is_attached() const REALM_NOEXCEPT
{
    return m_tree.is_attached();
}

template <class T, bool N>
ref_type TColumn<T,N>::get_ref() const REALM_NOEXCEPT
{
    return get_root_array()->get_ref();
}

template <class T, bool N>
void TColumn<T,N>::update_from_parent(std::size_t old_baseline) REALM_NOEXCEPT
{
    ColumnBaseWithIndex::update_from_parent(old_baseline);
    m_tree.update_from_parent(old_baseline);
}

template <class T, bool N>
MemRef TColumn<T,N>::clone_deep(Allocator& alloc) const
{
    return m_tree.clone_deep(alloc);
}

template <class T, bool N>
std::size_t TColumn<T,N>::size() const REALM_NOEXCEPT
{
    return m_tree.size();
}

template <class T, bool N>
T TColumn<T,N>::get(std::size_t ndx) const REALM_NOEXCEPT
{
    return m_tree.get(ndx);
}

template <class T, bool N>
T TColumn<T,N>::back() const REALM_NOEXCEPT
{
    return m_tree.back();
}

template <class T, bool N>
ref_type TColumn<T,N>::get_as_ref(std::size_t ndx) const REALM_NOEXCEPT
{
    return to_ref(get(ndx));
}

template <class T, bool N>
uint64_t TColumn<T,N>::get_uint(std::size_t ndx) const REALM_NOEXCEPT
{
    static_assert(std::is_convertible<T, uint64_t>::value, "T is not convertible to uint.");
    return static_cast<uint64_t>(get(ndx));
}

template <class T, bool N>
void TColumn<T,N>::add(T value)
{
    insert(npos, std::move(value));
}

template <class T, bool N>
void TColumn<T,N>::insert_without_updating_index(std::size_t row_ndx, T value, std::size_t num_rows)
{
    std::size_t size = this->size(); // Slow
    bool is_append = row_ndx == size || row_ndx == npos;
    std::size_t ndx_or_npos_if_append = is_append ? npos : row_ndx;

    m_tree.insert(ndx_or_npos_if_append, std::move(value), num_rows); // Throws
}

template <class T, bool N>
void TColumn<T,N>::insert(std::size_t row_ndx, T value, std::size_t num_rows)
{
    std::size_t size = this->size(); // Slow
    bool is_append = row_ndx == size || row_ndx == npos;
    std::size_t ndx_or_npos_if_append = is_append ? npos : row_ndx;

    m_tree.insert(ndx_or_npos_if_append, value, num_rows); // Throws

    if (has_search_index()) {
        row_ndx = is_append ? size : row_ndx;
        m_search_index->insert(row_ndx, value, num_rows, is_append); // Throws
    }
}

template <class T, bool N>
void TColumn<T,N>::erase_without_updating_index(std::size_t row_ndx, bool is_last)
{
    m_tree.erase(row_ndx, is_last);
}

template <class T, bool N>
void TColumn<T,N>::erase(std::size_t row_ndx)
{
    std::size_t last_row_ndx = size() - 1; // Note that size() is slow
    bool is_last = row_ndx == last_row_ndx;
    erase(row_ndx, is_last);
}

template <class T, bool N>
void TColumn<T, N>::move_last_over_without_updating_index(std::size_t row_ndx, std::size_t last_row_ndx)
{
    m_tree.move_last_over(row_ndx, last_row_ndx);
}

template <class T, bool N>
void TColumn<T,N>::move_last_over(std::size_t row_ndx, std::size_t last_row_ndx)
{
    REALM_ASSERT_3(row_ndx, <=, last_row_ndx);
    REALM_ASSERT_DEBUG(last_row_ndx + 1 == size());

    if (has_search_index()) {
        // remove the value to be overwritten from index
        bool is_last = true; // This tells StringIndex::erase() to not adjust subsequent indexes
        m_search_index->erase<StringData>(row_ndx, is_last); // Throws

        // update index to point to new location
        if (row_ndx != last_row_ndx) {
            int_fast64_t moved_value = get(last_row_ndx);
            m_search_index->update_ref(moved_value, last_row_ndx, row_ndx); // Throws
        }
    }

    move_last_over_without_updating_index(row_ndx, last_row_ndx);
}

template <class T, bool N>
void TColumn<T,N>::clear_without_updating_index()
{
    m_tree.clear(); // Throws
}

template <class T, bool N>
void TColumn<T,N>::clear()
{
    if (has_search_index()) {
        m_search_index->clear();
    }
    clear_without_updating_index();
}

// Implementing pure virtual method of ColumnBase.
template <class T, bool N>
void TColumn<T,N>::insert(std::size_t row_ndx, std::size_t num_rows, bool is_append)
{
    std::size_t row_ndx_2 = is_append ? realm::npos : row_ndx;
    T value{};
    insert(row_ndx_2, value, num_rows); // Throws
}

// Implementing pure virtual method of ColumnBase.
template <class T, bool N>
void TColumn<T,N>::erase(std::size_t row_ndx, bool is_last)
{
    if (has_search_index()) {
        m_search_index->erase<T>(row_ndx, is_last); // Throws
    }
    erase_without_updating_index(row_ndx, is_last); // Throws
}

// Implementing pure virtual method of ColumnBase.
template <class T, bool N>
void TColumn<T,N>::move_last_over(std::size_t row_ndx, std::size_t last_row_ndx, bool)
{
    move_last_over(row_ndx, last_row_ndx); // Throws
}

// Implementing pure virtual method of ColumnBase.
template <class T, bool N>
void TColumn<T,N>::clear(std::size_t, bool)
{
    clear(); // Throws
}


template <class T, bool N>
std::size_t TColumn<T,N>::lower_bound_int(T value) const REALM_NOEXCEPT
{
    if (root_is_leaf()) {
        return get_root_array()->lower_bound_int(value);
    }
    return ColumnBase::lower_bound(*this, value);
}

template <class T, bool N>
std::size_t TColumn<T,N>::upper_bound_int(T value) const REALM_NOEXCEPT
{
    if (root_is_leaf()) {
        return get_root_array()->upper_bound_int(value);
    }
    return ColumnBase::upper_bound(*this, value);
}

// For a *sorted* Column, return first element E for which E >= target or return -1 if none
template <class T, bool N>
std::size_t TColumn<T,N>::find_gte(T target, size_t start) const
{
    // fixme: slow reference implementation. See Array::FindGTE for faster version
    size_t ref = 0;
    size_t idx;
    for (idx = start; idx < size(); ++idx) {
        if (get(idx) >= target) {
            ref = idx;
            break;
        }
    }
    if (idx == size())
        ref = not_found;

    return ref;
}


template <class T, bool N>
bool TColumn<T,N>::compare_int(const TColumn<T,N>& c) const REALM_NOEXCEPT
{
    size_t n = size();
    if (c.size() != n)
        return false;
    for (size_t i=0; i<n; ++i) {
        if (get(i) != c.get(i))
            return false;
    }
    return true;
}

template <class T, bool N>
class TColumn<T,N>::CreateHandler: public ColumnBase::CreateHandler {
public:
    CreateHandler(Array::Type leaf_type, T value, Allocator& alloc):
        m_value(value), m_alloc(alloc), m_leaf_type(leaf_type) {}
    ref_type create_leaf(size_t size) override
    {
        MemRef mem = BpTree<T,N>::create_leaf(m_leaf_type, size, m_value, m_alloc); // Throws
        return mem.m_ref;
    }
private:
    const T m_value;
    Allocator& m_alloc;
    Array::Type m_leaf_type;
};

template <class T, bool N>
ref_type TColumn<T,N>::create(Allocator& alloc, Array::Type leaf_type, size_t size, T value)
{
    CreateHandler handler(leaf_type, std::move(value), alloc);
    return ColumnBase::create(alloc, size, handler);
}

template <class T, bool N>
ref_type TColumn<T,N>::write(std::size_t slice_offset, std::size_t slice_size,
                       std::size_t table_size, _impl::OutputStream& out) const
{
    return m_tree.write(slice_offset, slice_size, table_size, out);
}

template <class T, bool N>
void TColumn<T,N>::refresh_accessor_tree(size_t new_col_ndx, const Spec& spec)
{
    m_tree.init_from_parent();
    ColumnBaseWithIndex::refresh_accessor_tree(new_col_ndx, spec);
}

#if defined(REALM_DEBUG)
template <class T, bool N>
void TColumn<T,N>::Verify() const
{
    m_tree.verify();
}

template <class T, bool N>
void TColumn<T,N>::to_dot(std::ostream& out, StringData title) const
{
    ref_type ref = get_root_array()->get_ref();
    out << "subgraph cluster_integer_column" << ref << " {" << std::endl;
    out << " label = \"Integer column";
    if (title.size() != 0)
        out << "\\n'" << title << "'";
    out << "\";" << std::endl;
    tree_to_dot(out);
    out << "}" << std::endl;
}

template <class T, bool N>
void TColumn<T,N>::tree_to_dot(std::ostream& out) const
{
    ColumnBase::bptree_to_dot(get_root_array(), out);
}

template <class T, bool N>
void TColumn<T,N>::leaf_to_dot(MemRef leaf_mem, ArrayParent* parent, size_t ndx_in_parent,
                         std::ostream& out) const
{
    BpTree<T,N>::leaf_to_dot(leaf_mem, parent, ndx_in_parent, out, get_alloc());
}

template <class T, bool N>
MemStats TColumn<T,N>::stats() const
{
    MemStats stats;
    get_root_array()->stats(stats);
    return stats;
}


namespace _impl {
    void leaf_dumper(MemRef mem, Allocator& alloc, std::ostream& out, int level);
}

template <class T, bool N>
void TColumn<T,N>::do_dump_node_structure(std::ostream& out, int level) const
{
    dump_node_structure(*get_root_array(), out, level);
}

template <class T, bool N>
void TColumn<T,N>::dump_node_structure(const Array& root, std::ostream& out, int level)
{
    root.dump_bptree_structure(out, level, &_impl::leaf_dumper);
}
#endif // REALM_DEBUG


} // namespace realm

#endif // REALM_COLUMN_HPP
