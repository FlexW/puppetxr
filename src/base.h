#pragma once

#include "platform.h"

//
// Defer
//

template <typename F>
struct Defer
{
    F  func;
    b8 active;

    explicit Defer(F f) : func(f), active(true) {}
    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;
    Defer(Defer&& other) : func(other.func), active(other.active) { other.active = false; }

    ~Defer()
    {
        if (active)
        {
            func();
        }
    }
};

struct DeferHelper
{
};

template <typename F>
Defer<F> operator+(DeferHelper, F f)
{
    return Defer<F>(f);
}

#define DEFER_CONCAT2(a, b) a##b
#define DEFER_CONCAT(a, b)  DEFER_CONCAT2(a, b)
#define defer               const auto DEFER_CONCAT(_defer_, __COUNTER__) = DeferHelper{} + [&]()

//
// Array
//
// Elements must be trivially copyable: growth and removal move bytes.

template <typename T>
struct Array
{
    T*    data;
    usize length;
    usize allocated;

    void reserve (usize new_allocated)
    {
        if (new_allocated > allocated)
        {
            allocated = new_allocated;
            data      = (T*)os_memory_realloc(data, allocated * sizeof(T));
        }
    }

    void resize (usize new_length)
    {
        reserve(new_length);
        if (new_length > length)
        {
            memory_zero(data + length, (new_length - length) * sizeof(T));
        }
        length = new_length;
    }

    void release ()
    {
        os_memory_release(data);
        data      = nullptr;
        length    = 0;
        allocated = 0;
    }

    void clear ()
    {
        length = 0;
    }

    void insert (const T* new_items, usize index, usize new_count)
    {
        ASSERT(index <= length);
        if (length + new_count > allocated)
        {
            usize new_allocated = allocated ? allocated * 2 : 8;
            if (length + new_count > new_allocated)
            {
                new_allocated = length + new_count;
            }
            reserve(new_allocated);
        }

        memory_move(data + index + new_count, data + index, (length - index) * sizeof(T));
        length += new_count;

        if (new_items)
        {
            memory_copy(data + index, new_items, new_count * sizeof(T));
        }
        else
        {
            memory_zero(data + index, new_count * sizeof(T));
        }
    }

    void insert (T item, usize index)
    {
        insert(&item, index, 1);
    }

    T& add ()
    {
        insert(nullptr, length, 1);
        return last();
    }

    void add (T item)
    {
        insert(&item, length, 1);
    }

    void add (const T* new_items, usize new_count)
    {
        insert(new_items, length, new_count);
    }

    void remove (usize index, usize count = 1)
    {
        ASSERT(index + count <= length);
        memory_move(data + index, data + index + count, (length - index - count) * sizeof(T));
        length -= count;
    }

    void remove_swap (usize index)
    {
        ASSERT(index < length);
        if (index != length - 1)
        {
            data[index] = data[length - 1];
        }
        length--;
    }

    void pop ()
    {
        ASSERT(length > 0);
        length--;
    }

    b8 contains (const T& item, usize* out_index = nullptr) const
    {
        for (usize i = 0; i < length; i++)
        {
            if (memory_equal(&data[i], &item, sizeof(T)))
            {
                if (out_index)
                {
                    *out_index = i;
                }
                return true;
            }
        }
        return false;
    }

    T& first ()
    {
        ASSERT(length > 0);
        return data[0];
    }

    T& last ()
    {
        ASSERT(length > 0);
        return data[length - 1];
    }

    const T& first () const
    {
        ASSERT(length > 0);
        return data[0];
    }

    const T& last () const
    {
        ASSERT(length > 0);
        return data[length - 1];
    }

    T& operator[] (usize index)
    {
        ASSERT(index < length);
        return data[index];
    }

    const T& operator[] (usize index) const
    {
        ASSERT(index < length);
        return data[index];
    }

    T*       begin ()       { return data; }
    T*       end   ()       { return data + length; }
    const T* begin () const { return data; }
    const T* end   () const { return data + length; }
};

//
// Handle
//
// slot << 32 | generation. Generation 0 is never issued, so a zero value is null.

template <typename T>
struct Handle
{
    u64 value;

    Handle() : value(0) {}
    explicit Handle(u64 v) : value(v) {}

    explicit operator bool () const { return value != 0; }
    b8 operator== (Handle other) const { return value == other.value; }
    b8 operator!= (Handle other) const { return value != other.value; }
};

struct NullHandle
{
    template <typename T>
    operator Handle<T>() const // NOLINT(misc-explicit-constructor): converting to any handle type is its purpose
    {
        return {};
    }
};

inline constexpr NullHandle NULL_HANDLE = {};

//
// SparseArray
//
// Generational slot map. Values are packed in `dense`; handles stay valid across
// removals of other elements and are rejected after their own element is removed.
// Handles are of `Tag`, so that a backend can store its own struct behind a public opaque type.

template <typename T, typename Tag = T>
struct SparseArray
{
    struct Slot
    {
        u32 dense_index; // 1-based, 0 means free
        u32 generation;
    };

    Array<T>    dense;
    Array<Slot> slots;
    Array<u32>  dense_to_slot;
    Array<u32>  free_slots;

    void reserve (usize new_allocated)
    {
        dense.reserve(new_allocated);
        slots.reserve(new_allocated);
        dense_to_slot.reserve(new_allocated);
    }

    Handle<Tag> add (const T& value)
    {
        const Handle<Tag> handle = add();
        get(handle) = value;
        return handle;
    }

    Handle<Tag> add ()
    {
        u32 index = 0;
        if (free_slots.length != 0)
        {
            index = free_slots.last();
            free_slots.pop();
        }
        else
        {
            index = (u32)slots.length;
            slots.add({ .dense_index = 0, .generation = 1 });
        }

        dense.add();
        dense_to_slot.add(index);

        Slot& slot       = slots[index];
        slot.dense_index = (u32)dense.length;

        return make_handle(index, slot.generation);
    }

    void remove (Handle<Tag> handle)
    {
        if (!is_valid(handle))
        {
            return;
        }

        const u32 index       = slot_index(handle);
        Slot&     slot        = slots[index];
        const u32 dense_index = slot.dense_index - 1;
        const u32 last_index  = (u32)(dense.length - 1);

        if (dense_index != last_index)
        {
            dense[dense_index]                            = dense[last_index];
            dense_to_slot[dense_index]                    = dense_to_slot[last_index];
            slots[dense_to_slot[dense_index]].dense_index = dense_index + 1;
        }

        dense.pop();
        dense_to_slot.pop();

        slot.generation++;
        if (slot.generation == 0)
        {
            slot.generation = 1;
        }
        slot.dense_index = 0;
        free_slots.add(index);
    }

    b8 is_valid (Handle<Tag> handle) const
    {
        const u32 index = slot_index(handle);
        const u32 gen   = generation(handle);
        return gen != 0 && index < slots.length && slots[index].generation == gen && slots[index].dense_index != 0;
    }

    T& get (Handle<Tag> handle)
    {
        ASSERT(is_valid(handle));
        return dense[slots[slot_index(handle)].dense_index - 1];
    }

    const T& get (Handle<Tag> handle) const
    {
        ASSERT(is_valid(handle));
        return dense[slots[slot_index(handle)].dense_index - 1];
    }

    T* try_get (Handle<Tag> handle)
    {
        return is_valid(handle) ? &dense[slots[slot_index(handle)].dense_index - 1] : nullptr;
    }

    const T* try_get (Handle<Tag> handle) const
    {
        return is_valid(handle) ? &dense[slots[slot_index(handle)].dense_index - 1] : nullptr;
    }

    usize length () const
    {
        return dense.length;
    }

    void clear ()
    {
        dense.clear();
        slots.clear();
        dense_to_slot.clear();
        free_slots.clear();
    }

    void release ()
    {
        dense.release();
        slots.release();
        dense_to_slot.release();
        free_slots.release();
    }

    T*       data ()       { return dense.data; }
    const T* data () const { return dense.data; }

    Handle<Tag> handle_at (usize dense_index) const
    {
        ASSERT(dense_index < dense.length);
        const u32 index = dense_to_slot[dense_index];
        return make_handle(index, slots[index].generation);
    }

    static Handle<Tag> make_handle (u32 index, u32 gen) { return Handle<Tag>(((u64)index << 32) | gen); }
    static u32       slot_index  (Handle<Tag> handle)   { return (u32)(handle.value >> 32); }
    static u32       generation  (Handle<Tag> handle)   { return (u32)(handle.value & 0xFFFFFFFFu); }

    T& operator[] (Handle<Tag> handle)             { return get(handle); }
    const T& operator[] (Handle<Tag> handle) const { return get(handle); }

    T*       begin ()       { return dense.begin(); }
    T*       end   ()       { return dense.end(); }
    const T* begin () const { return dense.begin(); }
    const T* end   () const { return dense.end(); }
};

//
// Hashes
//

u64 hash     (const u8* key, usize size);
u32 hash_u32 (const u8* key, usize size);
u32 crc32    (const u8* data, usize size);

constexpr u64 hash_cstr (const char* str)
{
    u64 h = 0xCBF29CE484222325ull;
    for (usize i = 0; str[i]; i++)
    {
        h = (h ^ (u8)str[i]) * 0x100000001B3ull;
    }
    return h;
}

//
// HashMap
//
// Open addressing, linear probing, backward-shift erase. Keys are hashed by their
// bytes and compared with memory_equal, so K must be trivially copyable and a key
// whose bytes are all zero is reserved as the empty marker.

template <typename K, typename V>
struct HashMap
{
    struct Entry
    {
        K key;
        V value;
    };

    Entry* entries;
    usize  used;
    usize  capacity;

    void release ()
    {
        os_memory_release(entries);
        entries  = nullptr;
        used     = 0;
        capacity = 0;
    }

    void clear ()
    {
        used = 0;
        memory_zero(entries, capacity * sizeof(Entry));
    }

    void insert (const K& key, const V& value)
    {
        *at(key, true) = value;
    }

    V& get (const K& key)
    {
        V* value = at(key);
        ASSERT(value);
        return *value;
    }

    const V& get (const K& key) const
    {
        const V* value = at(key);
        ASSERT(value);
        return *value;
    }

    b8 contains (const K& key) const
    {
        return at(key) != nullptr;
    }

    V* at (const K& key, b8 create_if_needed = false)
    {
        ASSERT(!is_empty_key(key));

        if (create_if_needed && (used + 1) * 2 > capacity)
        {
            grow();
        }

        usize slot = 0;
        if (!find_slot(key, slot))
        {
            return nullptr;
        }

        if (is_empty_key(entries[slot].key))
        {
            if (!create_if_needed)
            {
                return nullptr;
            }
            used++;
            entries[slot].key = key;
            memory_zero(&entries[slot].value, sizeof(V));
        }

        return &entries[slot].value;
    }

    const V* at (const K& key) const
    {
        ASSERT(!is_empty_key(key));
        usize slot = 0;
        if (!find_slot(key, slot) || is_empty_key(entries[slot].key))
        {
            return nullptr;
        }
        return &entries[slot].value;
    }

    b8 erase (const K& key)
    {
        usize slot = 0;
        if (!find_slot(key, slot) || is_empty_key(entries[slot].key))
        {
            return false;
        }

        memory_zero(&entries[slot], sizeof(Entry));
        used--;

        usize next = (slot + 1) % capacity;
        while (!is_empty_key(entries[next].key))
        {
            const usize ideal = hash((const u8*)&entries[next].key, sizeof(K)) % capacity;
            if (probe_distance(ideal, slot) < probe_distance(ideal, next))
            {
                entries[slot] = entries[next];
                memory_zero(&entries[next], sizeof(Entry));
                slot = next;
            }
            next = (next + 1) % capacity;
        }
        return true;
    }

    V& operator[] (const K& key)
    {
        return *at(key, true);
    }

    const V& operator[] (const K& key) const
    {
        return get(key);
    }

private:
    static b8 is_empty_key (const K& key)
    {
        const u8* bytes = (const u8*)&key;
        for (usize i = 0; i < sizeof(K); i++)
        {
            if (bytes[i])
            {
                return false;
            }
        }
        return true;
    }

    void grow ()
    {
        HashMap grown = {};
        grown.capacity = capacity ? capacity * 2 : 16;
        grown.entries  = (Entry*)os_memory_alloc(grown.capacity * sizeof(Entry));

        for (usize i = 0; i < capacity; i++)
        {
            if (!is_empty_key(entries[i].key))
            {
                grown.insert(entries[i].key, entries[i].value);
            }
        }

        os_memory_release(entries);
        *this = grown;
    }

    b8 find_slot (const K& key, usize& out_slot) const
    {
        if (!capacity)
        {
            return false;
        }

        usize slot = hash((const u8*)&key, sizeof(K)) % capacity;
        while (!is_empty_key(entries[slot].key) && !memory_equal(&entries[slot].key, &key, sizeof(K)))
        {
            slot = (slot + 1) % capacity;
        }

        out_slot = slot;
        return true;
    }

    usize probe_distance (usize ideal, usize slot) const
    {
        return (slot + capacity - ideal) % capacity;
    }
};

//
// Json
//

struct JsonParser;
struct JsonArrayIterator;
struct JsonMemberIterator;
struct JsonMembers;

struct JsonValue
{
    const JsonParser* parser;
    u32               index;

    JsonArrayIterator begin      ()          const;
    JsonArrayIterator end        ()          const;
    JsonValue         operator[] (u32 at) const;
};

struct JsonArrayIterator
{
    const JsonParser* parser;
    u32               index;
    u32               remaining;

    JsonValue          operator*  () const;
    JsonArrayIterator& operator++ ();
    b8                 operator!= (const JsonArrayIterator& other) const;
};

struct JsonMember
{
    JsonValue key;
    JsonValue value;
};

struct JsonMemberIterator
{
    const JsonParser* parser;
    u32               index;
    u32               remaining;

    JsonMember          operator*  () const;
    JsonMemberIterator& operator++ ();
    b8                  operator!= (const JsonMemberIterator& other) const;
};

struct JsonMembers
{
    const JsonParser* parser;
    u32               object_index;

    JsonMemberIterator begin () const;
    JsonMemberIterator end   () const;
};

JsonParser* json_parse_from_file   (const char* path);
JsonParser* json_parse_from_string (const char* str);
void        json_release           (JsonParser* parser);

JsonValue   json_get_root          (const JsonParser* parser);

b8          json_is_valid          (const JsonValue& value);
b8          json_is_object         (const JsonValue& value);
b8          json_is_array          (const JsonValue& value);
b8          json_is_string         (const JsonValue& value);
b8          json_is_number         (const JsonValue& value);
b8          json_is_bool           (const JsonValue& value);
b8          json_is_null           (const JsonValue& value);

b8          json_as_bool           (const JsonValue& value, b8& out_value, b8 default_value = false);
b8          json_as_float          (const JsonValue& value, f32& out_value, f32 default_value = 0.0f);
b8          json_as_int            (const JsonValue& value, i32& out_value, i32 default_value = 0);
b8          json_as_uint           (const JsonValue& value, u32& out_value, u32 default_value = 0);
b8          json_as_string         (const JsonValue& value, char* out_value, usize* out_size, usize max_size, const char* default_value = nullptr);
b8          json_as_u64            (const JsonValue& value, u64& out_value, u64 default_value = 0); // plain decimal integers only, exact

b8          json_find_bool         (const JsonValue& object, const char* key, b8& out_value, b8 default_value = false);
b8          json_find_float        (const JsonValue& object, const char* key, f32& out_value, f32 default_value = 0.0f);
b8          json_find_int          (const JsonValue& object, const char* key, i32& out_value, i32 default_value = 0);
b8          json_find_uint         (const JsonValue& object, const char* key, u32& out_value, u32 default_value = 0);
b8          json_find_string       (const JsonValue& object, const char* key, char* out_value, usize* out_size, usize max_size, const char* default_value = nullptr);
b8          json_find_u64          (const JsonValue& object, const char* key, u64& out_value, u64 default_value = 0);

JsonValue   json_find              (const JsonValue& object, const char* key);
u32         json_length            (const JsonValue& array);
JsonValue   json_at                (const JsonValue& array, u32 index);
JsonMembers json_members           (const JsonValue& object);
b8          json_raw               (const JsonValue& value, const char*& out_text, usize& out_length); // source text, strings without quotes and still escaped
usize       json_format_f32        (char* dest, usize size, f32 value); // nine significant digits that json_as_float reads back bit-exactly; 0 for NaN and infinities
