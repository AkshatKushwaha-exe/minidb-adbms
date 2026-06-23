// Tests for Value, Schema serialisation and the Catalog.
#include "minidb/catalog/catalog.h"
#include "minidb/record/schema.h"
#include "minidb/record/value.h"
#include "test_framework.h"
#include "test_util.h"

using namespace minidb;

static Schema people_schema() {
    std::vector<Column> cols = {
        {"id", Type::INT}, {"name", Type::TEXT}, {"age", Type::INT}};
    return Schema(cols, /*pk=*/0);
}

TEST(value, int_and_text) {
    Value a = Value::make_int(42);
    Value b = Value::make_text("hi");
    CHECK_EQ(a.as_int(), (int64_t)42);
    CHECK_EQ(b.as_text(), std::string("hi"));
    CHECK(a.type() == Type::INT);
    CHECK(b.type() == Type::TEXT);
    CHECK_THROWS(a.as_text());
    CHECK_THROWS(b.as_int());
}

TEST(value, comparisons) {
    CHECK(Value::make_int(1) < Value::make_int(2));
    CHECK(Value::make_int(5) >= Value::make_int(5));
    CHECK(Value::make_text("apple") < Value::make_text("banana"));
    CHECK(Value::make_int(3) == Value::make_int(3));
    CHECK(Value::make_int(3) != Value::make_text("3"));
    // comparing across types must throw
    CHECK_THROWS((void)(Value::make_int(1) < Value::make_text("a")));
}

TEST(schema, serialize_roundtrip) {
    Schema s = people_schema();
    Tuple t = {Value::make_int(7), Value::make_text("grace hopper"),
               Value::make_int(85)};
    auto bytes = s.serialize(t);
    Tuple back = s.deserialize(bytes);
    CHECK_EQ(back.size(), (size_t)3);
    CHECK_EQ(back[0].as_int(), (int64_t)7);
    CHECK_EQ(back[1].as_text(), std::string("grace hopper"));
    CHECK_EQ(back[2].as_int(), (int64_t)85);
}

TEST(schema, empty_text_and_negative_int) {
    Schema s = people_schema();
    Tuple t = {Value::make_int(-12345), Value::make_text(""),
               Value::make_int(0)};
    auto back = s.deserialize(s.serialize(t));
    CHECK_EQ(back[0].as_int(), (int64_t)-12345);
    CHECK_EQ(back[1].as_text(), std::string(""));
}

TEST(schema, column_index_lookup) {
    Schema s = people_schema();
    CHECK_EQ(s.column_index("name"), 1);
    CHECK_EQ(s.column_index("missing"), -1);
}

TEST(schema, wrong_type_throws) {
    Schema s = people_schema();
    // id should be INT but we pass TEXT
    Tuple bad = {Value::make_text("oops"), Value::make_text("x"),
                 Value::make_int(1)};
    CHECK_THROWS(s.serialize(bad));
}

TEST(catalog, create_and_query) {
    Catalog cat;
    cat.create_table("people", people_schema());
    CHECK(cat.has_table("people"));
    CHECK(!cat.has_table("missing"));
    const TableInfo& t = cat.get_table("people");
    CHECK_EQ(t.schema.num_columns(), (size_t)3);
    // Auto primary-key index exists.
    CHECK_EQ(t.indexes.size(), (size_t)1);
    CHECK(t.indexes[0].primary);
    CHECK(t.indexes[0].unique);
    CHECK_THROWS(cat.create_table("people", people_schema()));  // dup
}

TEST(catalog, secondary_index) {
    Catalog cat;
    cat.create_table("people", people_schema());
    cat.add_index("people", {"people_name_idx", 1, false, false});
    const TableInfo& t = cat.get_table("people");
    CHECK_EQ(t.indexes.size(), (size_t)2);
    CHECK_THROWS(cat.add_index("nosuch", {"x", 0, false, false}));
}

TEST(catalog, save_load_roundtrip) {
    std::string path = minitest::temp_path("catalog.meta");
    {
        Catalog cat;
        cat.create_table("people", people_schema());
        cat.add_index("people", {"people_name_idx", 1, false, false});

        std::vector<Column> ocols = {{"oid", Type::INT}, {"item", Type::TEXT}};
        cat.create_table("orders", Schema(ocols, 0));
        cat.save(path);
    }
    {
        Catalog cat;
        cat.load(path);
        CHECK(cat.has_table("people"));
        CHECK(cat.has_table("orders"));
        const TableInfo& p = cat.get_table("people");
        CHECK_EQ(p.schema.num_columns(), (size_t)3);
        CHECK_EQ(p.schema.column(1).name, std::string("name"));
        CHECK_EQ(p.indexes.size(), (size_t)2);
        const TableInfo& o = cat.get_table("orders");
        CHECK_EQ(o.schema.column(1).type == Type::TEXT, true);
    }
}
