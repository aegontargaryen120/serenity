#pragma once

#include <cstdint>

namespace serenity::aot::runtime {

// This layout is mirrored by `%SnValue = type { i64, i64 }` in generated LLVM
// IR. The first word is a bytecode::ValueType tag; the second is its payload.
struct SnValue {
    std::uint64_t tag {0};
    std::uint64_t payload {0};
};

static_assert(sizeof(SnValue) == 16);
static_assert(alignof(SnValue) == alignof(std::uint64_t));

} // namespace serenity::aot::runtime

extern "C" {
void sn_rt_make(serenity::aot::runtime::SnValue* out, std::uint64_t tag, std::uint64_t payload);
void sn_rt_default(serenity::aot::runtime::SnValue* out, std::uint64_t tag);
void sn_rt_unary(serenity::aot::runtime::SnValue* out, std::int32_t opcode,
                 std::int32_t type, const serenity::aot::runtime::SnValue* input);
void sn_rt_binary(serenity::aot::runtime::SnValue* out, std::int32_t opcode,
                  std::int32_t type, const serenity::aot::runtime::SnValue* left,
                  const serenity::aot::runtime::SnValue* right);
void sn_rt_print(const serenity::aot::runtime::SnValue* value, bool newline);
void sn_rt_read(const serenity::aot::runtime::SnValue* stream, serenity::aot::runtime::SnValue* out);
void sn_rt_write(const serenity::aot::runtime::SnValue* stream,
                 const serenity::aot::runtime::SnValue* value);
void* sn_rt_struct_new(std::int32_t field_count);
void sn_rt_struct_default(void* object, std::int32_t field_index, std::uint64_t type_tag);
void sn_rt_struct_set_value(void* object, std::int32_t field_index,
                            std::uint64_t tag, std::uint64_t payload);
void sn_rt_struct_get(const serenity::aot::runtime::SnValue* object, std::int32_t field_index,
                      serenity::aot::runtime::SnValue* out);
void sn_rt_struct_set(const serenity::aot::runtime::SnValue* object, std::int32_t field_index,
                      const serenity::aot::runtime::SnValue* value);
std::int32_t sn_rt_exit_code(const serenity::aot::runtime::SnValue* value);
}
