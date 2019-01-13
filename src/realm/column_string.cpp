/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <cstdlib>
#include <cstring>
#include <cstdio> // debug
#include <iomanip>
#include <ostream>

#include <memory>

#include <realm/query_conditions.hpp>
#include <realm/column_string.hpp>
#include <realm/index_string.hpp>
#include <realm/table.hpp>

using namespace realm;
using namespace realm::util;


namespace {

const size_t small_string_max_size = 15;  // ArrayString
const size_t medium_string_max_size = 63; // ArrayStringLong

void copy_leaf(const ArrayString& from, ArrayStringLong& to)
{
    size_t n = from.size();
    for (size_t i = 0; i != n; ++i) {
        StringData str = from.get(i);
        to.add(str); // Throws
    }
}

void copy_leaf(const ArrayString& from, ArrayBigBlobs& to)
{
    size_t n = from.size();
    for (size_t i = 0; i != n; ++i) {
        StringData str = from.get(i);
        to.add_string(str); // Throws
    }
}

void copy_leaf(const ArrayStringLong& from, ArrayBigBlobs& to)
{
    size_t n = from.size();
    for (size_t i = 0; i != n; ++i) {
        StringData str = from.get(i);
        to.add_string(str); // Throws
    }
}

} // anonymous namespace


StringColumn::StringColumn(Allocator& alloc, ref_type ref, bool nullable, size_t column_ndx)
    : ColumnBaseSimple(column_ndx)
    , m_nullable(nullable)
{
    char* header = alloc.translate(ref);
    MemRef mem(header, ref, alloc);

    // Within an StringColumn the leaves can be of different
    // type optimized for the lengths of the strings contained
    // therein.  The type is indicated by the combination of the
    // is_inner_bptree_node(N), has_refs(R) and context_flag(C):
    //
    //   N R C
    //   1 0 0   InnerBptreeNode (not leaf)
    //   0 0 0   ArrayString
    //   0 1 0   ArrayStringLong
    //   0 1 1   ArrayBigBlobs
    Array::Type type = Array::get_type_from_header(header);
    switch (type) {
        case Array::type_Normal: {
            // Small strings root leaf
            ArrayString* root = new ArrayString(alloc, nullable); // Throws
            root->init_from_mem(mem);
            m_array.reset(root);
            return;
        }
        case Array::type_HasRefs: {
            bool is_big = Array::get_context_flag_from_header(header);
            if (!is_big) {
                // Medium strings root leaf
                ArrayStringLong* root = new ArrayStringLong(alloc, nullable); // Throws
                root->init_from_mem(mem);
                m_array.reset(root);
                return;
            }
            // Big strings root leaf
            ArrayBigBlobs* root = new ArrayBigBlobs(alloc, nullable); // Throws
            root->init_from_mem(mem);
            m_array.reset(root);
            return;
        }
        case Array::type_InnerBptreeNode: {
            // Non-leaf root
            Array* root = new Array(alloc); // Throws
            root->init_from_mem(mem);
            m_array.reset(root);
            return;
        }
    }
    REALM_ASSERT_DEBUG(false);
}


StringColumn::~StringColumn() noexcept
{
}


void StringColumn::destroy() noexcept
{
    ColumnBaseSimple::destroy();
    if (m_search_index)
        m_search_index->destroy();
}

bool StringColumn::is_nullable() const noexcept
{
    return m_nullable;
}

StringData StringColumn::get(size_t ndx) const noexcept
{
    REALM_ASSERT_DEBUG(ndx < size());

    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            return leaf->get(ndx);
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            return leaf->get(ndx);
        }
        // Big strings root leaf
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        return leaf->get_string(ndx);
    }

    // Non-leaf root
    std::pair<MemRef, size_t> p = static_cast<BpTreeNode*>(m_array.get())->get_bptree_leaf(ndx);
    const char* leaf_header = p.first.get_addr();
    size_t ndx_in_leaf = p.second;
    bool long_strings = Array::get_hasrefs_from_header(leaf_header);
    if (!long_strings) {
        // Small strings
        return ArrayString::get(leaf_header, ndx_in_leaf, m_nullable);
    }
    Allocator& alloc = m_array->get_alloc();
    bool is_big = Array::get_context_flag_from_header(leaf_header);
    if (!is_big) {
        // Medimum strings
        return ArrayStringLong::get(leaf_header, ndx_in_leaf, alloc, m_nullable);
    }
    // Big strings
    return ArrayBigBlobs::get_string(leaf_header, ndx_in_leaf, alloc, m_nullable);
}

bool StringColumn::is_null(size_t ndx) const noexcept
{
#ifdef REALM_DEBUG
    StringData sd = get(ndx);
    REALM_ASSERT_DEBUG(m_nullable || !sd.is_null());
    return sd.is_null();
#else
    return m_nullable && get(ndx).is_null();
#endif
}

StringData StringColumn::get_index_data(size_t ndx, StringIndex::StringConversionBuffer&) const noexcept
{
    return get(ndx);
}


void StringColumn::set_null(size_t ndx)
{
    if (!m_nullable) {
        throw LogicError{LogicError::column_not_nullable};
    }
    StringData sd = realm::null();
    set(ndx, sd);
}

void StringColumn::populate_search_index()
{
    REALM_ASSERT(m_search_index);

    size_t num_rows = size();
    for (size_t row_ndx = 0; row_ndx != num_rows; ++row_ndx) {
        StringData value = get(row_ndx);
        size_t num_rows_to_insert = 1;
        bool is_append = true;
        m_search_index->insert(row_ndx, value, num_rows_to_insert, is_append); // Throws
    }
}

StringIndex* StringColumn::create_search_index()
{
    REALM_ASSERT(!m_search_index);

    std::unique_ptr<StringIndex> index;
    index.reset(new StringIndex(this, m_array->get_alloc())); // Throws

    // Populate the index
    m_search_index = std::move(index);
    populate_search_index();
    return m_search_index.get();
}


void StringColumn::destroy_search_index() noexcept
{
    m_search_index.reset();
}


std::unique_ptr<StringIndex> StringColumn::release_search_index() noexcept
{
    return std::move(m_search_index);
}


void StringColumn::set_search_index_ref(ref_type ref, ArrayParent* parent, size_t ndx_in_parent)
{
    REALM_ASSERT(!m_search_index);
    m_search_index.reset(new StringIndex(ref, parent, ndx_in_parent, this, m_array->get_alloc())); // Throws
}


void StringColumn::set_ndx_in_parent(size_t ndx_in_parent) noexcept
{
    m_array->set_ndx_in_parent(ndx_in_parent);
    if (m_search_index) {
        m_search_index->set_ndx_in_parent(ndx_in_parent + 1);
    }
}


void StringColumn::update_from_parent(size_t old_baseline) noexcept
{
    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            leaf->update_from_parent(old_baseline);
        }
        else {
            bool is_big = m_array->get_context_flag();
            if (!is_big) {
                // Medium strings root leaf
                ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
                leaf->update_from_parent(old_baseline);
            }
            else {
                // Big strings root leaf
                ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
                leaf->update_from_parent(old_baseline);
            }
        }
    }
    else {
        // Non-leaf root
        m_array->update_from_parent(old_baseline);
    }
    if (m_search_index)
        m_search_index->update_from_parent(old_baseline);
}


namespace {

class SetLeafElem : public BpTreeNode::UpdateHandler {
public:
    Allocator& m_alloc;
    const StringData m_value;
    bool m_nullable;

    SetLeafElem(Allocator& alloc, StringData value, bool nullable) noexcept
        : m_alloc(alloc)
        , m_value(value)
        , m_nullable(nullable)
    {
    }

    void update(MemRef mem, ArrayParent* parent, size_t ndx_in_parent, size_t elem_ndx_in_leaf) override
    {
        bool long_strings = Array::get_hasrefs_from_header(mem.get_addr());
        if (long_strings) {
            bool is_big = Array::get_context_flag_from_header(mem.get_addr());
            if (is_big) {
                ArrayBigBlobs leaf(m_alloc, m_nullable);
                leaf.init_from_mem(mem);
                leaf.set_parent(parent, ndx_in_parent);
                leaf.set_string(elem_ndx_in_leaf, m_value); // Throws
                return;
            }
            ArrayStringLong leaf(m_alloc, m_nullable);
            leaf.init_from_mem(mem);
            leaf.set_parent(parent, ndx_in_parent);
            if (m_value.size() <= medium_string_max_size) {
                leaf.set(elem_ndx_in_leaf, m_value); // Throws
                return;
            }
            // Upgrade leaf from medium to big strings
            ArrayBigBlobs new_leaf(m_alloc, m_nullable);
            new_leaf.create();                          // Throws
            new_leaf.set_parent(parent, ndx_in_parent); // Throws
            new_leaf.update_parent();                   // Throws
            copy_leaf(leaf, new_leaf);                  // Throws
            leaf.destroy();
            new_leaf.set_string(elem_ndx_in_leaf, m_value); // Throws
            return;
        }
        ArrayString leaf(m_alloc, m_nullable);
        leaf.init_from_mem(mem);
        leaf.set_parent(parent, ndx_in_parent);
        if (m_value.size() <= small_string_max_size) {
            leaf.set(elem_ndx_in_leaf, m_value); // Throws
            return;
        }
        if (m_value.size() <= medium_string_max_size) {
            // Upgrade leaf from small to medium strings
            ArrayStringLong new_leaf(m_alloc, m_nullable);
            new_leaf.create(); // Throws
            new_leaf.set_parent(parent, ndx_in_parent);
            new_leaf.update_parent();  // Throws
            copy_leaf(leaf, new_leaf); // Throws
            leaf.destroy();
            new_leaf.set(elem_ndx_in_leaf, m_value); // Throws
            return;
        }
        // Upgrade leaf from small to big strings
        ArrayBigBlobs new_leaf(m_alloc, m_nullable);
        new_leaf.create(); // Throws
        new_leaf.set_parent(parent, ndx_in_parent);
        new_leaf.update_parent();  // Throws
        copy_leaf(leaf, new_leaf); // Throws
        leaf.destroy();
        new_leaf.set_string(elem_ndx_in_leaf, m_value); // Throws
    }
};

} // anonymous namespace

void StringColumn::set(size_t ndx, StringData value)
{
    REALM_ASSERT_DEBUG(ndx < size());

    // We must modify the search index before modifying the column, because we
    // need to be able to abort the operation if the modification of the search
    // index fails due to a unique constraint violation.

    // Update search index
    // (it is important here that we do it before actually setting
    //  the value, or the index would not be able to find the correct
    //  position to update (as it looks for the old value))
    if (m_search_index) {
        m_search_index->set(ndx, value); // Throws
    }

    bool array_root_is_leaf = !m_array->is_inner_bptree_node();
    if (array_root_is_leaf) {
        LeafType leaf_type = upgrade_root_leaf(value.size()); // Throws
        switch (leaf_type) {
            case leaf_type_Small: {
                ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
                leaf->set(ndx, value); // Throws
                return;
            }
            case leaf_type_Medium: {
                ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
                leaf->set(ndx, value); // Throws
                return;
            }
            case leaf_type_Big: {
                ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
                leaf->set_string(ndx, value); // Throws
                return;
            }
        }
        REALM_ASSERT(false);
    }

    SetLeafElem set_leaf_elem(m_array->get_alloc(), value, m_nullable);
    static_cast<BpTreeNode*>(m_array.get())->update_bptree_elem(ndx, set_leaf_elem); // Throws
}


class StringColumn::EraseLeafElem : public BpTreeNode::EraseHandler {
public:
    StringColumn& m_column;
    EraseLeafElem(StringColumn& column, bool nullable) noexcept
        : m_column(column)
        , m_nullable(nullable)
    {
    }
    bool erase_leaf_elem(MemRef leaf_mem, ArrayParent* parent, size_t leaf_ndx_in_parent,
                         size_t elem_ndx_in_leaf) override
    {
        bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
        if (!long_strings) {
            // Small strings
            ArrayString leaf(m_column.get_alloc(), m_nullable);
            leaf.init_from_mem(leaf_mem);
            leaf.set_parent(parent, leaf_ndx_in_parent);
            REALM_ASSERT_3(leaf.size(), >=, 1);
            size_t last_ndx = leaf.size() - 1;
            if (last_ndx == 0)
                return true;
            size_t ndx = elem_ndx_in_leaf;
            if (ndx == npos)
                ndx = last_ndx;
            leaf.erase(ndx); // Throws
            return false;
        }
        bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
        if (!is_big) {
            // Medium strings
            ArrayStringLong leaf(m_column.get_alloc(), m_nullable);
            leaf.init_from_mem(leaf_mem);
            leaf.set_parent(parent, leaf_ndx_in_parent);
            REALM_ASSERT_3(leaf.size(), >=, 1);
            size_t last_ndx = leaf.size() - 1;
            if (last_ndx == 0)
                return true;
            size_t ndx = elem_ndx_in_leaf;
            if (ndx == npos)
                ndx = last_ndx;
            leaf.erase(ndx); // Throws
            return false;
        }
        // Big strings
        ArrayBigBlobs leaf(m_column.get_alloc(), m_nullable);
        leaf.init_from_mem(leaf_mem);
        leaf.set_parent(parent, leaf_ndx_in_parent);
        REALM_ASSERT_3(leaf.size(), >=, 1);
        size_t last_ndx = leaf.size() - 1;
        if (last_ndx == 0)
            return true;
        size_t ndx = elem_ndx_in_leaf;
        if (ndx == npos)
            ndx = last_ndx;
        leaf.erase(ndx); // Throws
        return false;
    }
    void destroy_leaf(MemRef leaf_mem) noexcept override
    {
        Array::destroy_deep(leaf_mem, m_column.get_alloc());
    }
    void replace_root_by_leaf(MemRef leaf_mem) override
    {
        std::unique_ptr<Array> leaf;
        bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
        if (!long_strings) {
            // Small strings
            ArrayString* leaf_2 = new ArrayString(m_column.get_alloc(), m_nullable); // Throws
            leaf_2->init_from_mem(leaf_mem);
            leaf.reset(leaf_2);
        }
        else {
            bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
            if (!is_big) {
                // Medium strings
                ArrayStringLong* leaf_2 = new ArrayStringLong(m_column.get_alloc(), m_nullable); // Throws
                leaf_2->init_from_mem(leaf_mem);
                leaf.reset(leaf_2);
            }
            else {
                // Big strings
                ArrayBigBlobs* leaf_2 = new ArrayBigBlobs(m_column.get_alloc(), m_nullable); // Throws
                leaf_2->init_from_mem(leaf_mem);
                leaf.reset(leaf_2);
            }
        }
        m_column.replace_root_array(std::move(leaf)); // Throws, but accessor ownership is passed to callee
    }
    void replace_root_by_empty_leaf() override
    {
        std::unique_ptr<ArrayString> leaf;
        leaf.reset(new ArrayString(m_column.get_alloc(), m_nullable)); // Throws
        leaf->create();                                                // Throws
        m_column.replace_root_array(std::move(leaf)); // Throws, but accessor ownership is passed to callee
    }

private:
    bool m_nullable;
};

void StringColumn::do_erase(size_t ndx, bool is_last)
{
    REALM_ASSERT_3(ndx, <, size());
    REALM_ASSERT_3(is_last, ==, (ndx == size() - 1));

    // Update search index
    // (it is important here that we do it before actually setting
    //  the value, or the index would not be able to find the correct
    //  position to update (as it looks for the old value))
    if (m_search_index) {
        m_search_index->erase<StringData>(ndx, is_last);
    }

    bool array_root_is_leaf = !m_array->is_inner_bptree_node();
    if (array_root_is_leaf) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            leaf->erase(ndx); // Throws
            return;
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            leaf->erase(ndx); // Throws
            return;
        }
        // Big strings root leaf
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        leaf->erase(ndx); // Throws
        return;
    }

    // Non-leaf root
    size_t ndx_2 = is_last ? npos : ndx;
    EraseLeafElem erase_leaf_elem(*this, m_nullable);
    BpTreeNode::erase_bptree_elem(static_cast<BpTreeNode*>(m_array.get()), ndx_2, erase_leaf_elem); // Throws
}


void StringColumn::do_move_last_over(size_t row_ndx, size_t last_row_ndx)
{
    REALM_ASSERT_3(row_ndx, <=, last_row_ndx);
    REALM_ASSERT_3(last_row_ndx + 1, ==, size());

    // FIXME: ExceptionSafety: The current implementation of this
    // function is not exception-safe, and it is hard to see how to
    // repair it.

    // FIXME: Consider doing two nested calls to
    // update_bptree_elem(). If the two leaves are not the same, no
    // copying is needed. If they are the same, call
    // Array::move_last_over() (does not yet
    // exist). Array::move_last_over() could be implemented in a way
    // that avoids the intermediate copy. This approach is also likely
    // to be necesseray for exception safety.

    StringData value = get(last_row_ndx);

    // Copying string data from a column to itself requires an
    // intermediate copy of the data (constr:bptree-copy-to-self).
    std::unique_ptr<char[]> buffer(new char[value.size()]); // Throws
    realm::safe_copy_n(value.data(), value.size(), buffer.get());
    StringData copy_of_value(value.is_null() ? nullptr : buffer.get(), value.size());

    if (m_search_index) {
        // remove the value to be overwritten from index
        bool is_last = true; // This tells StringIndex::erase() to not adjust subsequent indexes
        m_search_index->erase<StringData>(row_ndx, is_last); // Throws

        // update index to point to new location
        if (row_ndx != last_row_ndx)
            m_search_index->update_ref(copy_of_value, last_row_ndx, row_ndx); // Throws
    }

    bool array_root_is_leaf = !m_array->is_inner_bptree_node();
    if (array_root_is_leaf) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            leaf->set(row_ndx, copy_of_value); // Throws
            leaf->erase(last_row_ndx);         // Throws
            return;
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            leaf->set(row_ndx, copy_of_value); // Throws
            leaf->erase(last_row_ndx);         // Throws
            return;
        }
        // Big strings root leaf
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        leaf->set_string(row_ndx, copy_of_value); // Throws
        leaf->erase(last_row_ndx);                // Throws
        return;
    }

    // Non-leaf root
    BpTreeNode* node = static_cast<BpTreeNode*>(m_array.get());
    SetLeafElem set_leaf_elem(node->get_alloc(), copy_of_value, m_nullable);
    node->update_bptree_elem(row_ndx, set_leaf_elem); // Throws
    EraseLeafElem erase_leaf_elem(*this, m_nullable);
    BpTreeNode::erase_bptree_elem(node, realm::npos, erase_leaf_elem); // Throws
}

void StringColumn::do_swap_rows(size_t row_ndx_1, size_t row_ndx_2)
{
    REALM_ASSERT_3(row_ndx_1, <=, size());
    REALM_ASSERT_3(row_ndx_2, <=, size());
    REALM_ASSERT_DEBUG(row_ndx_1 != row_ndx_2);

    StringData value_1 = get(row_ndx_1);
    StringData value_2 = get(row_ndx_2);

    if (value_1.is_null() && value_2.is_null()) {
        return;
    }

    std::string buffer_1{value_1.data(), value_1.size()};
    std::string buffer_2{value_2.data(), value_2.size()};

    if (value_1.is_null()) {
        set(row_ndx_2, realm::null());
    }
    else {
        StringData copy{buffer_1.data(), buffer_1.size()};
        set(row_ndx_2, copy);
    }

    if (value_2.is_null()) {
        set(row_ndx_1, realm::null());
    }
    else {
        StringData copy{buffer_2.data(), buffer_2.size()};
        set(row_ndx_1, copy);
    }
}


void StringColumn::do_clear()
{
    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            leaf->clear(); // Throws
        }
        else {
            bool is_big = m_array->get_context_flag();
            if (!is_big) {
                // Medium strings root leaf
                ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
                leaf->clear(); // Throws
            }
            else {
                // Big strings root leaf
                ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
                leaf->clear(); // Throws
            }
        }
    }
    else {
        // Non-leaf root - revert to small strings leaf
        Allocator& alloc = m_array->get_alloc();
        std::unique_ptr<ArrayString> array;
        array.reset(new ArrayString(alloc, m_nullable)); // Throws
        array->create();                                 // Throws
        array->set_parent(m_array->get_parent(), m_array->get_ndx_in_parent());
        array->update_parent(); // Throws

        // Remove original node
        m_array->destroy_deep();
        m_array = std::move(array);
    }

    if (m_search_index)
        m_search_index->clear(); // Throws
}


size_t StringColumn::count(StringData value) const
{
    if (m_search_index)
        return m_search_index->count(value); // Throws

    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            return leaf->count(value);
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            return leaf->count(value);
        }
        // Big strings root leaf
        BinaryData bin(value.data(), value.size());
        bool is_string = true;
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        return leaf->count(bin, is_string);
    }

    // Non-leaf root
    size_t num_matches = 0;

    // FIXME: It would be better to always require that 'end' is
    // specified explicitely, since Table has the size readily
    // available, and Array::get_bptree_size() is deprecated.
    BpTreeNode* node = static_cast<BpTreeNode*>(m_array.get());
    size_t begin = 0, end = node->get_bptree_size();
    while (begin < end) {
        std::pair<MemRef, size_t> p = node->get_bptree_leaf(begin);
        MemRef leaf_mem = p.first;
        REALM_ASSERT_3(p.second, ==, 0);
        bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
        if (!long_strings) {
            // Small strings
            ArrayString leaf(m_array->get_alloc(), m_nullable);
            leaf.init_from_mem(leaf_mem);
            num_matches += leaf.count(value);
            begin += leaf.size();
            continue;
        }
        bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
        if (!is_big) {
            // Medium strings
            ArrayStringLong leaf(m_array->get_alloc(), m_nullable);
            leaf.init_from_mem(leaf_mem);
            num_matches += leaf.count(value);
            begin += leaf.size();
            continue;
        }
        // Big strings
        ArrayBigBlobs leaf(m_array->get_alloc(), m_nullable);
        leaf.init_from_mem(leaf_mem);
        BinaryData bin(value.data(), value.size());
        bool is_string = true;
        num_matches += leaf.count(bin, is_string);
        begin += leaf.size();
    }

    return num_matches;
}


size_t StringColumn::find_first(StringData value, size_t begin, size_t end) const
{
    REALM_ASSERT_3(begin, <=, size());
    REALM_ASSERT(end == npos || (begin <= end && end <= size()));

    if (m_search_index && begin == 0 && end == npos)
        return m_search_index->find_first(value); // Throws

    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            return leaf->find_first(value, begin, end);
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            return leaf->find_first(value, begin, end);
        }
        // Big strings root leaf
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        BinaryData bin(value.data(), value.size());
        bool is_string = true;
        return leaf->find_first(bin, is_string, begin, end);
    }

    // Non-leaf root
    //
    // FIXME: It would be better to always require that 'end' is
    // specified explicitly, since Table has the size readily
    // available, and Array::get_bptree_size() is deprecated.
    BpTreeNode* node = static_cast<BpTreeNode*>(m_array.get());
    if (end == npos)
        end = node->get_bptree_size();

    size_t ndx_in_tree = begin;
    while (ndx_in_tree < end) {
        std::pair<MemRef, size_t> p = node->get_bptree_leaf(ndx_in_tree);
        MemRef leaf_mem = p.first;
        size_t ndx_in_leaf = p.second, end_in_leaf;
        size_t leaf_offset = ndx_in_tree - ndx_in_leaf;
        bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
        if (!long_strings) {
            // Small strings
            ArrayString leaf(m_array->get_alloc(), m_nullable);
            leaf.init_from_mem(leaf_mem);
            end_in_leaf = std::min(leaf.size(), end - leaf_offset);
            size_t ndx = leaf.find_first(value, ndx_in_leaf, end_in_leaf);
            if (ndx != not_found)
                return leaf_offset + ndx;
        }
        else {
            bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
            if (!is_big) {
                // Medium strings
                ArrayStringLong leaf(m_array->get_alloc(), m_nullable);
                leaf.init_from_mem(leaf_mem);
                end_in_leaf = std::min(leaf.size(), end - leaf_offset);
                size_t ndx = leaf.find_first(value, ndx_in_leaf, end_in_leaf);
                if (ndx != not_found)
                    return leaf_offset + ndx;
            }
            else {
                // Big strings
                ArrayBigBlobs leaf(m_array->get_alloc(), m_nullable);
                leaf.init_from_mem(leaf_mem);
                end_in_leaf = std::min(leaf.size(), end - leaf_offset);
                BinaryData bin(value.data(), value.size());
                bool is_string = true;
                size_t ndx = leaf.find_first(bin, is_string, ndx_in_leaf, end_in_leaf);
                if (ndx != not_found)
                    return leaf_offset + ndx;
            }
        }
        ndx_in_tree = leaf_offset + end_in_leaf;
    }

    return not_found;
}


void StringColumn::find_all(IntegerColumn& result, StringData value, size_t begin, size_t end) const
{
    REALM_ASSERT_3(begin, <=, size());
    REALM_ASSERT(end == npos || (begin <= end && end <= size()));

    if (m_search_index && begin == 0 && end == npos) {
        m_search_index->find_all(result, value); // Throws
        return;
    }

    if (root_is_leaf()) {
        size_t leaf_offset = 0;
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            leaf->find_all(result, value, leaf_offset, begin, end); // Throws
            return;
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            leaf->find_all(result, value, leaf_offset, begin, end); // Throws
            return;
        }
        // Big strings root leaf
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        BinaryData bin(value.data(), value.size());
        bool is_string = true;
        leaf->find_all(result, bin, is_string, leaf_offset, begin, end); // Throws
        return;
    }

    // Non-leaf root
    //
    // FIXME: It would be better to always require that 'end' is
    // specified explicitely, since Table has the size readily
    // available, and Array::get_bptree_size() is deprecated.
    BpTreeNode* node = static_cast<BpTreeNode*>(m_array.get());
    if (end == npos)
        end = node->get_bptree_size();

    size_t ndx_in_tree = begin;
    while (ndx_in_tree < end) {
        std::pair<MemRef, size_t> p = node->get_bptree_leaf(ndx_in_tree);
        MemRef leaf_mem = p.first;
        size_t ndx_in_leaf = p.second, end_in_leaf;
        size_t leaf_offset = ndx_in_tree - ndx_in_leaf;
        bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
        if (!long_strings) {
            // Small strings
            ArrayString leaf(m_array->get_alloc(), m_nullable);
            leaf.init_from_mem(leaf_mem);
            end_in_leaf = std::min(leaf.size(), end - leaf_offset);
            leaf.find_all(result, value, leaf_offset, ndx_in_leaf, end_in_leaf); // Throws
        }
        else {
            bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
            if (!is_big) {
                // Medium strings
                ArrayStringLong leaf(m_array->get_alloc(), m_nullable);
                leaf.init_from_mem(leaf_mem);
                end_in_leaf = std::min(leaf.size(), end - leaf_offset);
                leaf.find_all(result, value, leaf_offset, ndx_in_leaf, end_in_leaf); // Throws
            }
            else {
                // Big strings
                ArrayBigBlobs leaf(m_array->get_alloc(), m_nullable);
                leaf.init_from_mem(leaf_mem);
                end_in_leaf = std::min(leaf.size(), end - leaf_offset);
                BinaryData bin(value.data(), value.size());
                bool is_string = true;
                leaf.find_all(result, bin, is_string, leaf_offset, ndx_in_leaf, end_in_leaf); // Throws
            }
        }
        ndx_in_tree = leaf_offset + end_in_leaf;
    }
}


FindRes StringColumn::find_all_no_copy(StringData value, InternalFindResult& result) const
{
    REALM_ASSERT(m_search_index);

    if (value.is_null() && !m_nullable) {
        // Early out if looking for null but we aren't nullable.
        return FindRes::FindRes_not_found;
    }

    return m_search_index->find_all_no_copy(value, result);
}


namespace {

struct BinToStrAdaptor {
    typedef StringData value_type;
    const ArrayBigBlobs& m_big_blobs;
    BinToStrAdaptor(const ArrayBigBlobs& big_blobs) noexcept
        : m_big_blobs(big_blobs)
    {
    }
    ~BinToStrAdaptor() noexcept
    {
    }
    size_t size() const noexcept
    {
        return m_big_blobs.size();
    }
    StringData get(size_t ndx) const noexcept
    {
        return m_big_blobs.get_string(ndx);
    }
};

} // anonymous namespace

size_t StringColumn::lower_bound_string(StringData value) const noexcept
{
    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            return ColumnBase::lower_bound(*leaf, value);
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            return ColumnBase::lower_bound(*leaf, value);
        }
        // Big strings root leaf
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        BinToStrAdaptor adapt(*leaf);
        return ColumnBase::lower_bound(adapt, value);
    }
    // Non-leaf root
    return ColumnBase::lower_bound(*this, value);
}

size_t StringColumn::upper_bound_string(StringData value) const noexcept
{
    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            return ColumnBase::upper_bound(*leaf, value);
        }
        bool is_big = m_array->get_context_flag();
        if (!is_big) {
            // Medium strings root leaf
            ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
            return ColumnBase::upper_bound(*leaf, value);
        }
        // Big strings root leaf
        ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
        BinToStrAdaptor adapt(*leaf);
        return ColumnBase::upper_bound(adapt, value);
    }
    // Non-leaf root
    return ColumnBase::upper_bound(*this, value);
}


bool StringColumn::auto_enumerate(ref_type& keys_ref, ref_type& values_ref, bool enforce) const
{
    Allocator& alloc = m_array->get_alloc();
    ref_type keys_ref_2 = StringColumn::create(alloc); // Throws
    StringColumn keys(alloc, keys_ref_2, m_nullable);  // Throws // FIXME

    // Generate list of unique values (keys)
    size_t n = size();
    for (size_t i = 0; i != n; ++i) {
        StringData v = get(i);

        // Insert keys in sorted order, ignoring duplicates
        size_t pos = keys.lower_bound_string(v);
        if (pos != keys.size() && keys.get(pos) == v)
            continue;

        // Don't bother auto enumerating if there are too few duplicates
        if (!enforce && n / 2 < keys.size()) {
            keys.destroy(); // cleanup
            return false;
        }

        keys.insert(pos, v); // Throws
    }

    // Generate enumerated list of entries
    ref_type values_ref_2 = IntegerColumn::create(alloc); // Throws
    IntegerColumn values(alloc, values_ref_2);            // Throws
    for (size_t i = 0; i != n; ++i) {
        StringData v = get(i);
        size_t pos = keys.lower_bound_string(v);
        REALM_ASSERT_3(pos, !=, keys.size());
        values.add(pos); // Throws
    }

    keys_ref = keys.get_ref();
    values_ref = values.get_ref();
    return true;
}


bool StringColumn::compare_string(const StringColumn& c) const
{
    size_t n = size();
    if (c.size() != n)
        return false;
    for (size_t i = 0; i != n; ++i) {
        StringData v_1 = get(i);
        StringData v_2 = c.get(i);
        if (v_1 != v_2)
            return false;
    }
    return true;
}


void StringColumn::do_insert(size_t row_ndx, StringData value, size_t num_rows)
{
    bptree_insert(row_ndx, value, num_rows); // Throws

    if (m_search_index) {
        bool is_append = row_ndx == realm::npos;
        size_t row_ndx_2 = is_append ? size() - num_rows : row_ndx;
        m_search_index->insert(row_ndx_2, value, num_rows, is_append); // Throws
    }
}


void StringColumn::do_insert(size_t row_ndx, StringData value, size_t num_rows, bool is_append)
{
    size_t row_ndx_2 = is_append ? realm::npos : row_ndx;
    bptree_insert(row_ndx_2, value, num_rows); // Throws

    if (m_search_index)
        m_search_index->insert(row_ndx, value, num_rows, is_append); // Throws
}


void StringColumn::bptree_insert(size_t row_ndx, StringData value, size_t num_rows)
{
    REALM_ASSERT(row_ndx == realm::npos || row_ndx < size());
    ref_type new_sibling_ref = 0;
    BpTreeNode::TreeInsert<StringColumn> state;
    for (size_t i = 0; i != num_rows; ++i) {
        size_t row_ndx_2 = row_ndx == realm::npos ? realm::npos : row_ndx + i;
        if (root_is_leaf()) {
            REALM_ASSERT(row_ndx_2 == realm::npos || row_ndx_2 < REALM_MAX_BPNODE_SIZE);
            LeafType leaf_type = upgrade_root_leaf(value.size()); // Throws
            switch (leaf_type) {
                case leaf_type_Small: {
                    // Small strings root leaf
                    ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
                    new_sibling_ref = leaf->bptree_leaf_insert(row_ndx_2, value, state); // Throws
                    break;
                }
                case leaf_type_Medium: {
                    // Medium strings root leaf
                    ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
                    new_sibling_ref = leaf->bptree_leaf_insert(row_ndx_2, value, state); // Throws
                    break;
                }
                case leaf_type_Big: {
                    // Big strings root leaf
                    ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
                    new_sibling_ref = leaf->bptree_leaf_insert_string(row_ndx_2, value, state); // Throws
                    break;
                }
            }
        }
        else {
            // Non-leaf root
            BpTreeNode* node = static_cast<BpTreeNode*>(m_array.get());
            state.m_value = value;
            state.m_nullable = m_nullable;
            if (row_ndx_2 == realm::npos) {
                new_sibling_ref = node->bptree_append(state); // Throws
            }
            else {
                new_sibling_ref = node->bptree_insert(row_ndx_2, state); // Throws
            }
        }

        if (REALM_UNLIKELY(new_sibling_ref)) {
            bool is_append = row_ndx_2 == realm::npos;
            introduce_new_root(new_sibling_ref, state, is_append); // Throws
        }
    }
}


ref_type StringColumn::leaf_insert(MemRef leaf_mem, ArrayParent& parent, size_t ndx_in_parent, Allocator& alloc,
                                   size_t insert_ndx, BpTreeNode::TreeInsert<StringColumn>& state)
{
    bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
    if (long_strings) {
        bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
        if (is_big) {
            ArrayBigBlobs leaf(alloc, state.m_nullable);
            leaf.init_from_mem(leaf_mem);
            leaf.set_parent(&parent, ndx_in_parent);
            return leaf.bptree_leaf_insert_string(insert_ndx, state.m_value, state); // Throws
        }
        ArrayStringLong leaf(alloc, state.m_nullable);
        leaf.init_from_mem(leaf_mem);
        leaf.set_parent(&parent, ndx_in_parent);
        if (state.m_value.size() <= medium_string_max_size)
            return leaf.bptree_leaf_insert(insert_ndx, state.m_value, state); // Throws
        // Upgrade leaf from medium to big strings
        ArrayBigBlobs new_leaf(alloc, state.m_nullable);
        new_leaf.create(); // Throws
        new_leaf.set_parent(&parent, ndx_in_parent);
        new_leaf.update_parent();  // Throws
        copy_leaf(leaf, new_leaf); // Throws
        leaf.destroy();
        return new_leaf.bptree_leaf_insert_string(insert_ndx, state.m_value, state); // Throws
    }
    ArrayString leaf(alloc, state.m_nullable);
    leaf.init_from_mem(leaf_mem);
    leaf.set_parent(&parent, ndx_in_parent);
    if (state.m_value.size() <= small_string_max_size)
        return leaf.bptree_leaf_insert(insert_ndx, state.m_value, state); // Throws
    if (state.m_value.size() <= medium_string_max_size) {
        // Upgrade leaf from small to medium strings
        ArrayStringLong new_leaf(alloc, state.m_nullable);
        new_leaf.create(); // Throws
        new_leaf.set_parent(&parent, ndx_in_parent);
        new_leaf.update_parent();  // Throws
        copy_leaf(leaf, new_leaf); // Throws
        leaf.destroy();
        return new_leaf.bptree_leaf_insert(insert_ndx, state.m_value, state); // Throws
    }
    // Upgrade leaf from small to big strings
    ArrayBigBlobs new_leaf(alloc, state.m_nullable);
    new_leaf.create(); // Throws
    new_leaf.set_parent(&parent, ndx_in_parent);
    new_leaf.update_parent();  // Throws
    copy_leaf(leaf, new_leaf); // Throws
    leaf.destroy();
    return new_leaf.bptree_leaf_insert_string(insert_ndx, state.m_value, state); // Throws
}


StringColumn::LeafType StringColumn::upgrade_root_leaf(size_t value_size)
{
    REALM_ASSERT(root_is_leaf());

    bool long_strings = m_array->has_refs();
    if (long_strings) {
        bool is_big = m_array->get_context_flag();
        if (is_big)
            return leaf_type_Big;
        if (value_size <= medium_string_max_size)
            return leaf_type_Medium;
        // Upgrade root leaf from medium to big strings
        ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
        std::unique_ptr<ArrayBigBlobs> new_leaf;
        ArrayParent* parent = leaf->get_parent();
        size_t ndx_in_parent = leaf->get_ndx_in_parent();
        Allocator& alloc = leaf->get_alloc();
        new_leaf.reset(new ArrayBigBlobs(alloc, m_nullable)); // Throws
        new_leaf->create();                                   // Throws
        new_leaf->set_parent(parent, ndx_in_parent);
        new_leaf->update_parent();   // Throws
        copy_leaf(*leaf, *new_leaf); // Throws
        leaf->destroy();
        m_array = std::move(new_leaf);
        return leaf_type_Big;
    }
    if (value_size <= small_string_max_size)
        return leaf_type_Small;
    ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
    ArrayParent* parent = leaf->get_parent();
    size_t ndx_in_parent = leaf->get_ndx_in_parent();
    Allocator& alloc = leaf->get_alloc();
    if (value_size <= medium_string_max_size) {
        // Upgrade root leaf from small to medium strings
        std::unique_ptr<ArrayStringLong> new_leaf;
        new_leaf.reset(new ArrayStringLong(alloc, m_nullable)); // Throws
        new_leaf->create();                                     // Throws
        new_leaf->set_parent(parent, ndx_in_parent);
        new_leaf->update_parent();   // Throws
        copy_leaf(*leaf, *new_leaf); // Throws
        leaf->destroy();
        m_array = std::move(new_leaf);
        return leaf_type_Medium;
    }
    // Upgrade root leaf from small to big strings
    std::unique_ptr<ArrayBigBlobs> new_leaf;
    new_leaf.reset(new ArrayBigBlobs(alloc, m_nullable)); // Throws
    new_leaf->create();                                   // Throws
    new_leaf->set_parent(parent, ndx_in_parent);
    new_leaf->update_parent();   // Throws
    copy_leaf(*leaf, *new_leaf); // Throws
    leaf->destroy();
    m_array = std::move(new_leaf);
    return leaf_type_Big;
}


std::unique_ptr<const ArrayParent> StringColumn::get_leaf(size_t ndx, size_t& out_ndx_in_leaf,
                                                          LeafType& out_leaf_type) const
{
    size_t off;
    ArrayParent* ap = nullptr;
    out_leaf_type = get_block(ndx, &ap, off, false);
    out_ndx_in_leaf = ndx - off;
    return std::unique_ptr<const ArrayParent>(ap);
}

StringColumn::LeafType StringColumn::get_block(size_t ndx, ArrayParent** ap, size_t& off, bool use_retval) const
{
    static_cast<void>(use_retval);
    REALM_ASSERT_3(use_retval, ==, false); // retval optimization not supported. See Array on how to implement

    Allocator& alloc = m_array->get_alloc();
    if (root_is_leaf()) {
        off = 0;
        bool long_strings = m_array->has_refs();
        if (long_strings) {
            if (m_array->get_context_flag()) {
                ArrayBigBlobs* asb2 = new ArrayBigBlobs(alloc, m_nullable); // Throws
                asb2->init_from_mem(m_array->get_mem());
                *ap = asb2;
                return leaf_type_Big;
            }
            ArrayStringLong* asl2 = new ArrayStringLong(alloc, m_nullable); // Throws
            asl2->init_from_mem(m_array->get_mem());
            *ap = asl2;
            return leaf_type_Medium;
        }
        ArrayString* as2 = new ArrayString(alloc, m_nullable); // Throws
        as2->init_from_mem(m_array->get_mem());
        *ap = as2;
        return leaf_type_Small;
    }

    BpTreeNode* node = static_cast<BpTreeNode*>(m_array.get());
    std::pair<MemRef, size_t> p = node->get_bptree_leaf(ndx);
    off = ndx - p.second;
    bool long_strings = Array::get_hasrefs_from_header(p.first.get_addr());
    if (long_strings) {
        if (Array::get_context_flag_from_header(p.first.get_addr())) {
            ArrayBigBlobs* asb2 = new ArrayBigBlobs(alloc, m_nullable);
            asb2->init_from_mem(p.first);
            *ap = asb2;
            return leaf_type_Big;
        }
        ArrayStringLong* asl2 = new ArrayStringLong(alloc, m_nullable);
        asl2->init_from_mem(p.first);
        *ap = asl2;
        return leaf_type_Medium;
    }
    ArrayString* as2 = new ArrayString(alloc, m_nullable);
    as2->init_from_mem(p.first);
    *ap = as2;
    return leaf_type_Small;
}


class StringColumn::CreateHandler : public ColumnBase::CreateHandler {
public:
    CreateHandler(Allocator& alloc)
        : m_alloc(alloc)
    {
    }
    ref_type create_leaf(size_t size) override
    {
        MemRef mem = ArrayString::create_array(size, m_alloc); // Throws
        return mem.get_ref();
    }

private:
    Allocator& m_alloc;
};

ref_type StringColumn::create(Allocator& alloc, size_t size)
{
    CreateHandler handler(alloc);
    return ColumnBase::create(alloc, size, handler);
}


class StringColumn::SliceHandler : public ColumnBase::SliceHandler {
public:
    SliceHandler(Allocator& alloc, bool nullable)
        : m_alloc(alloc)
        , m_nullable(nullable)
    {
    }
    MemRef slice_leaf(MemRef leaf_mem, size_t offset, size_t size, Allocator& target_alloc) override
    {
        bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
        if (!long_strings) {
            // Small strings
            ArrayString leaf(m_alloc, m_nullable);
            leaf.init_from_mem(leaf_mem);
            return leaf.slice(offset, size, target_alloc); // Throws
        }
        bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
        if (!is_big) {
            // Medium strings
            ArrayStringLong leaf(m_alloc, m_nullable);
            leaf.init_from_mem(leaf_mem);
            return leaf.slice(offset, size, target_alloc); // Throws
        }
        // Big strings
        ArrayBigBlobs leaf(m_alloc, m_nullable);
        leaf.init_from_mem(leaf_mem);
        return leaf.slice(offset, size, target_alloc); // Throws
    }

private:
    Allocator& m_alloc;
    bool m_nullable;
};

ref_type StringColumn::write(size_t slice_offset, size_t slice_size, size_t table_size,
                             _impl::OutputStream& out) const
{
    ref_type ref;
    if (root_is_leaf()) {
        Allocator& alloc = Allocator::get_default();
        MemRef mem;
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            mem = leaf->slice(slice_offset, slice_size, alloc); // Throws
        }
        else {
            bool is_big = m_array->get_context_flag();
            if (!is_big) {
                // Medium strings
                ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
                mem = leaf->slice(slice_offset, slice_size, alloc); // Throws
            }
            else {
                // Big strings
                ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
                mem = leaf->slice(slice_offset, slice_size, alloc); // Throws
            }
        }
        Array slice(alloc);
        _impl::DeepArrayDestroyGuard dg(&slice);
        slice.init_from_mem(mem);
        bool deep = true;                               // Deep
        bool only_if_modified = false;                  // Always
        ref = slice.write(out, deep, only_if_modified); // Throws
    }
    else {
        SliceHandler handler(get_alloc(), m_nullable);
        ref = ColumnBaseSimple::write(m_array.get(), slice_offset, slice_size, table_size, handler, out); // Throws
    }
    return ref;
}


void StringColumn::refresh_accessor_tree(size_t col_ndx, const Spec& spec)
{
    ColumnBaseSimple::refresh_accessor_tree(col_ndx, spec);
    refresh_root_accessor(); // Throws

    // Refresh search index
    if (m_search_index) {
        size_t ndx_in_parent = m_array->get_ndx_in_parent();
        size_t search_ndx_in_parent = m_search_index->get_ndx_in_parent();
        // Index in parent should have been set before now, if it is incorrect we will
        // fix it now, but we have probably already written to an incorrect index at this point.
        REALM_ASSERT_DEBUG_EX(search_ndx_in_parent == ndx_in_parent + 1, search_ndx_in_parent, ndx_in_parent + 1);
        m_search_index->refresh_accessor_tree(col_ndx, spec); // Throws
    }
}


void StringColumn::refresh_root_accessor()
{
    // The type of the cached root array accessor may no longer match the
    // underlying root node. In that case we need to replace it. Note that when
    // the root node is an inner B+-tree node, then only the top array accessor
    // of that node is cached. The top array accessor of an inner B+-tree node
    // is of type Array.

    ref_type root_ref = m_array->get_ref_from_parent();
    MemRef root_mem(root_ref, m_array->get_alloc());
    bool new_root_is_leaf = !Array::get_is_inner_bptree_node_from_header(root_mem.get_addr());
    bool new_root_is_small = !Array::get_hasrefs_from_header(root_mem.get_addr());
    bool new_root_is_medium = !Array::get_context_flag_from_header(root_mem.get_addr());
    bool old_root_is_leaf = !m_array->is_inner_bptree_node();
    bool old_root_is_small = !m_array->has_refs();
    bool old_root_is_medium = !m_array->get_context_flag();

    bool root_type_changed = old_root_is_leaf != new_root_is_leaf ||
                             (old_root_is_leaf && (old_root_is_small != new_root_is_small ||
                                                   (!old_root_is_small && old_root_is_medium != new_root_is_medium)));
    if (!root_type_changed) {
        // Keep, but refresh old root accessor
        if (old_root_is_leaf) {
            if (old_root_is_small) {
                // Root is 'small strings' leaf
                ArrayString* root = static_cast<ArrayString*>(m_array.get());
                root->init_from_parent();
                return;
            }
            if (old_root_is_medium) {
                // Root is 'medium strings' leaf
                ArrayStringLong* root = static_cast<ArrayStringLong*>(m_array.get());
                root->init_from_parent();
                return;
            }
            // Root is 'big strings' leaf
            ArrayBigBlobs* root = static_cast<ArrayBigBlobs*>(m_array.get());
            root->init_from_parent();
            return;
        }
        // Root is inner node
        Array* root = m_array.get();
        root->init_from_parent();
        return;
    }

    // Create new root accessor
    Array* new_root;
    Allocator& alloc = m_array->get_alloc();
    if (new_root_is_leaf) {
        if (new_root_is_small) {
            // New root is 'small strings' leaf
            ArrayString* root = new ArrayString(alloc, m_nullable); // Throws
            root->init_from_mem(root_mem);
            new_root = root;
        }
        else if (new_root_is_medium) {
            // New root is 'medium strings' leaf
            ArrayStringLong* root = new ArrayStringLong(alloc, m_nullable); // Throws
            root->init_from_mem(root_mem);
            new_root = root;
        }
        else {
            // New root is 'big strings' leaf
            ArrayBigBlobs* root = new ArrayBigBlobs(alloc, m_nullable); // Throws
            root->init_from_mem(root_mem);
            new_root = root;
        }
    }
    else {
        // New root is inner node
        Array* root = new Array(alloc); // Throws
        root->init_from_mem(root_mem);
        new_root = root;
    }
    new_root->set_parent(m_array->get_parent(), m_array->get_ndx_in_parent());

    // Instate new root
    m_array.reset(new_root);
}

// LCOV_EXCL_START ignore debug functions

#ifdef REALM_DEBUG

namespace {

size_t verify_leaf(MemRef mem, Allocator& alloc)
{
    // fixme, null support (validation will still run for nullable leafs, but just not include
    // any validation of the null properties)
    bool long_strings = Array::get_hasrefs_from_header(mem.get_addr());
    if (!long_strings) {
        // Small strings
        ArrayString leaf(alloc, false);
        leaf.init_from_mem(mem);
        leaf.verify();
        return leaf.size();
    }
    bool is_big = Array::get_context_flag_from_header(mem.get_addr());
    if (!is_big) {
        // Medium strings
        ArrayStringLong leaf(alloc, false);
        leaf.init_from_mem(mem);
        leaf.verify();
        return leaf.size();
    }
    // Big strings
    ArrayBigBlobs leaf(alloc, false);
    leaf.init_from_mem(mem);
    leaf.verify();
    return leaf.size();
}

} // anonymous namespace

#endif

void StringColumn::verify() const
{
#ifdef REALM_DEBUG
    if (root_is_leaf()) {
        bool long_strings = m_array->has_refs();
        if (!long_strings) {
            // Small strings root leaf
            ArrayString* leaf = static_cast<ArrayString*>(m_array.get());
            leaf->verify();
        }
        else {
            bool is_big = m_array->get_context_flag();
            if (!is_big) {
                // Medium strings root leaf
                ArrayStringLong* leaf = static_cast<ArrayStringLong*>(m_array.get());
                leaf->verify();
            }
            else {
                // Big strings root leaf
                ArrayBigBlobs* leaf = static_cast<ArrayBigBlobs*>(m_array.get());
                leaf->verify();
            }
        }
    }
    else {
        // Non-leaf root
        m_array->verify_bptree(&verify_leaf);
    }

    if (m_search_index) {
        m_search_index->verify();
        m_search_index->verify_entries(*this);
    }
#endif
}


void StringColumn::verify(const Table& table, size_t col_ndx) const
{
#ifdef REALM_DEBUG
    ColumnBase::verify(table, col_ndx);

    typedef _impl::TableFriend tf;
    const Spec& spec = tf::get_spec(table);
    ColumnAttr attr = spec.get_column_attr(col_ndx);
    bool column_has_search_index = (attr & col_attr_Indexed) != 0;
    REALM_ASSERT_3(column_has_search_index, ==, bool(m_search_index));
    if (column_has_search_index) {
        REALM_ASSERT(m_search_index->get_ndx_in_parent() == get_root_array()->get_ndx_in_parent() + 1);
    }
#else
    static_cast<void>(table);
    static_cast<void>(col_ndx);
#endif
}


void StringColumn::to_dot(std::ostream& out, StringData title) const
{
#ifdef REALM_DEBUG
    ref_type ref = m_array->get_ref();
    out << "subgraph cluster_string_column" << ref << " {" << std::endl;
    out << " label = \"String column";
    if (title.size() != 0)
        out << "\\n'" << dot_escape_quote(title) << "'";
    out << "\";" << std::endl;
    tree_to_dot(out);
    out << "}" << std::endl;
#else
    static_cast<void>(out);
    static_cast<void>(title);
#endif
}

void StringColumn::leaf_to_dot(MemRef leaf_mem, ArrayParent* parent, size_t ndx_in_parent, std::ostream& out) const
{
#ifdef REALM_DEBUG
    bool long_strings = Array::get_hasrefs_from_header(leaf_mem.get_addr());
    if (!long_strings) {
        // Small strings
        ArrayString leaf(m_array->get_alloc(), m_nullable);
        leaf.init_from_mem(leaf_mem);
        leaf.set_parent(parent, ndx_in_parent);
        leaf.to_dot(out);
        return;
    }
    bool is_big = Array::get_context_flag_from_header(leaf_mem.get_addr());
    if (!is_big) {
        // Medium strings
        ArrayStringLong leaf(m_array->get_alloc(), m_nullable);
        leaf.init_from_mem(leaf_mem);
        leaf.set_parent(parent, ndx_in_parent);
        leaf.to_dot(out);
        return;
    }
    // Big strings
    ArrayBigBlobs leaf(m_array->get_alloc(), m_nullable);
    leaf.init_from_mem(leaf_mem);
    leaf.set_parent(parent, ndx_in_parent);
    bool is_strings = true;
    leaf.to_dot(out, is_strings);
#else
    static_cast<void>(leaf_mem);
    static_cast<void>(parent);
    static_cast<void>(ndx_in_parent);
    static_cast<void>(out);
#endif
}

#ifdef REALM_DEBUG

namespace {

void leaf_dumper(MemRef mem, Allocator& alloc, std::ostream& out, int level)
{
    // todo, support null (will now just show up in dump as empty strings)
    size_t leaf_size;
    const char* leaf_type;
    bool long_strings = Array::get_hasrefs_from_header(mem.get_addr());
    if (!long_strings) {
        // Small strings
        ArrayString leaf(alloc, false);
        leaf.init_from_mem(mem);
        leaf_size = leaf.size();
        leaf_type = "Small strings leaf";
    }
    else {
        bool is_big = Array::get_context_flag_from_header(mem.get_addr());
        if (!is_big) {
            // Medium strings
            ArrayStringLong leaf(alloc, false);
            leaf.init_from_mem(mem);
            leaf_size = leaf.size();
            leaf_type = "Medimum strings leaf";
        }
        else {
            // Big strings
            ArrayBigBlobs leaf(alloc, false);
            leaf.init_from_mem(mem);
            leaf_size = leaf.size();
            leaf_type = "Big strings leaf";
        }
    }
    int indent = level * 2;
    out << std::setw(indent) << "" << leaf_type << " (size: " << leaf_size << ")\n";
}

} // anonymous namespace

#endif

void StringColumn::do_dump_node_structure(std::ostream& out, int level) const
{
#ifdef REALM_DEBUG
    m_array->dump_bptree_structure(out, level, &leaf_dumper);
    int indent = level * 2;
    out << std::setw(indent) << ""
        << "Search index\n";
    m_search_index->do_dump_node_structure(out, level + 1);
#else
    static_cast<void>(out);
    static_cast<void>(level);
#endif
}

// LCOV_EXCL_STOP ignore debug functions
