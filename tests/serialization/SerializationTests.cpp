// Serialization tests: JSON round trips, binary stream encoding and the
// versioned header contract.
#include "doctest.h"

#include "local3d/serialization/BinaryStream.hpp"
#include "local3d/serialization/Json.hpp"

#include <string>

using namespace l3d;
using namespace l3d::serial;

TEST_SUITE("serialization.json") {
    TEST_CASE("parses scalars") {
        const auto parsed = JsonValue::Parse(R"({"a": 1, "b": 2.5, "c": true, "d": null, "e": "text"})");
        REQUIRE(parsed.HasValue());
        const JsonValue& root = *parsed;
        CHECK(root["a"].AsInt() == 1);
        CHECK(root["b"].AsNumber() == doctest::Approx(2.5));
        CHECK(root["c"].AsBool());
        CHECK(root["d"].IsNull());
        CHECK(root["e"].AsString() == "text");
    }

    TEST_CASE("parses arrays and nested objects") {
        const auto parsed = JsonValue::Parse(R"({"items": [1, 2, 3], "nested": {"x": [true, false]}})");
        REQUIRE(parsed.HasValue());
        const JsonValue& root = *parsed;
        CHECK(root["items"].Size() == 3);
        CHECK(root["items"][1].AsInt() == 2);
        CHECK(root["nested"]["x"][0].AsBool());
        CHECK_FALSE(root["nested"]["x"][1].AsBool());
    }

    TEST_CASE("handles escapes") {
        const auto parsed = JsonValue::Parse(R"({"text": "line\nbreak\t\"quoted\" \u0041"})");
        REQUIRE(parsed.HasValue());
        CHECK((*parsed)["text"].AsString() == "line\nbreak\t\"quoted\" A");
    }

    TEST_CASE("reports errors with a line number") {
        const auto broken = JsonValue::Parse("{\n  \"a\": 1,\n  \"b\": \n}");
        REQUIRE(broken.IsError());
        CHECK(broken.Error().Code() == StatusCode::ParseError);

        CHECK(JsonValue::Parse("{").IsError());
        CHECK(JsonValue::Parse("[1,]").IsError());
        CHECK(JsonValue::Parse("nope").IsError());
        CHECK(JsonValue::Parse("{} trailing").IsError());
    }

    TEST_CASE("empty containers") {
        const auto parsed = JsonValue::Parse(R"({"a": [], "b": {}})");
        REQUIRE(parsed.HasValue());
        CHECK((*parsed)["a"].IsArray());
        CHECK((*parsed)["a"].Size() == 0);
        CHECK((*parsed)["b"].IsObject());
    }

    TEST_CASE("missing keys return null instead of crashing") {
        const auto parsed = JsonValue::Parse(R"({"a": 1})");
        REQUIRE(parsed.HasValue());
        CHECK((*parsed)["missing"].IsNull());
        CHECK((*parsed)["missing"].AsInt(42) == 42);
        CHECK((*parsed)["missing"].AsString("fallback") == "fallback");
        CHECK((*parsed).Contains("a"));
        CHECK_FALSE((*parsed).Contains("b"));
    }

    TEST_CASE("builds and dumps documents") {
        JsonValue root = JsonValue::MakeObject();
        root.Set("name", JsonValue("Local3D"));
        root.Set("version", JsonValue(3));
        JsonValue list = JsonValue::MakeArray();
        list.Push(JsonValue(1.5));
        list.Push(JsonValue("two"));
        root.Set("items", std::move(list));

        const std::string text = root.Dump(2);
        const auto reparsed = JsonValue::Parse(text);
        REQUIRE(reparsed.HasValue());
        CHECK((*reparsed)["name"].AsString() == "Local3D");
        CHECK((*reparsed)["version"].AsInt() == 3);
        CHECK((*reparsed)["items"].Size() == 2);
        CHECK((*reparsed)["items"][1].AsString() == "two");
    }

    TEST_CASE("escapes control characters on output") {
        JsonValue value(std::string("tab\there\nnewline"));
        const std::string text = value.Dump(0);
        CHECK(text == "\"tab\\there\\nnewline\"");
        const auto reparsed = JsonValue::Parse(text);
        REQUIRE(reparsed.HasValue());
        CHECK(reparsed->AsString() == "tab\there\nnewline");
    }
}

TEST_SUITE("serialization.binary") {
    TEST_CASE("round trips every primitive") {
        BinaryWriter writer;
        writer.WriteU8(200);
        writer.WriteI32(-123456);
        writer.WriteU32(4000000000u);
        writer.WriteI64(-9000000000LL);
        writer.WriteU64(18000000000000000000ULL);
        writer.WriteF32(3.14159f);
        writer.WriteF64(2.718281828459045);
        writer.WriteString("hello world");
        writer.WriteVarUint(0);
        writer.WriteVarUint(127);
        writer.WriteVarUint(128);
        writer.WriteVarUint(12345678901234ULL);

        BinaryReader reader(std::as_bytes(std::span(writer.Bytes().data(), writer.Bytes().size())));
        CHECK(reader.ReadU8() == 200);
        CHECK(reader.ReadI32() == -123456);
        CHECK(reader.ReadU32() == 4000000000u);
        CHECK(reader.ReadI64() == -9000000000LL);
        CHECK(reader.ReadU64() == 18000000000000000000ULL);
        CHECK(reader.ReadF32() == doctest::Approx(3.14159f));
        CHECK(reader.ReadF64() == doctest::Approx(2.718281828459045));
        CHECK(reader.ReadString() == "hello world");
        CHECK(reader.ReadVarUint() == 0);
        CHECK(reader.ReadVarUint() == 127);
        CHECK(reader.ReadVarUint() == 128);
        CHECK(reader.ReadVarUint() == 12345678901234ULL);
        CHECK_FALSE(reader.HasError());
        CHECK(reader.Remaining() == 0);
    }

    TEST_CASE("varints are compact") {
        BinaryWriter small;
        small.WriteVarUint(1);
        CHECK(small.Size() == 1);
        BinaryWriter large;
        large.WriteVarUint(300);
        CHECK(large.Size() == 2);
    }

    TEST_CASE("reading past the end sets an error instead of reading garbage") {
        BinaryWriter writer;
        writer.WriteU32(7);
        BinaryReader reader(std::as_bytes(std::span(writer.Bytes().data(), writer.Bytes().size())));
        CHECK(reader.ReadU32() == 7);
        CHECK(reader.ReadU32() == 0);
        CHECK(reader.HasError());
        CHECK(reader.ReadU8() == 0);
        CHECK(reader.ReadString().empty());
    }

    TEST_CASE("truncated strings are rejected") {
        BinaryWriter writer;
        writer.WriteVarUint(100); // Claims 100 bytes that are not there.
        writer.WriteString("short");
        BinaryReader reader(std::as_bytes(std::span(writer.Bytes().data(), writer.Bytes().size())));
        CHECK(reader.ReadString().empty());
        CHECK(reader.HasError());
    }

    TEST_CASE("raw struct round trip") {
        struct Payload {
            i32 a = 0;
            f32 b = 0.0f;
        };
        BinaryWriter writer;
        const Payload original{42, 1.5f};
        writer.WriteRaw(std::as_bytes(std::span(&original, 1)));

        BinaryReader reader(std::as_bytes(std::span(writer.Bytes().data(), writer.Bytes().size())));
        const Payload restored = reader.ReadRaw<Payload>();
        CHECK_FALSE(reader.HasError());
        CHECK(restored.a == 42);
        CHECK(restored.b == doctest::Approx(1.5f));
    }

    TEST_CASE("versioned header validates magic and version") {
        constexpr u32 kMagic = 0x4C334441; // "L3DA"
        BinaryWriter writer;
        WriteVersionedHeader(writer, kMagic, 2);
        writer.WriteString("payload");

        BinaryReader reader(std::as_bytes(std::span(writer.Bytes().data(), writer.Bytes().size())));
        const auto header = ReadVersionedHeader(reader, kMagic, 2);
        REQUIRE(header.HasValue());
        CHECK(*header == 2);
        CHECK(reader.ReadString() == "payload");

        BinaryReader wrongMagic(
            std::as_bytes(std::span(writer.Bytes().data(), writer.Bytes().size())));
        const auto rejected = ReadVersionedHeader(wrongMagic, 0xDEADBEEF, 2);
        REQUIRE(rejected.IsError());
        CHECK(rejected.Error().Code() == StatusCode::ParseError);

        BinaryReader tooNew(std::as_bytes(std::span(writer.Bytes().data(), writer.Bytes().size())));
        const auto unsupported = ReadVersionedHeader(tooNew, kMagic, 1);
        REQUIRE(unsupported.IsError());
        CHECK(unsupported.Error().Code() == StatusCode::Unsupported);
    }
}
