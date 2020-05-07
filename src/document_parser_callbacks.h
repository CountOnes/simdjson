#ifndef SIMDJSON_DOCUMENT_PARSER_CALLBACKS_H
#define SIMDJSON_DOCUMENT_PARSER_CALLBACKS_H

#include "simdjson.h"

namespace simdjson {
namespace dom {

//
// Parser callbacks
//

inline void parser::init_stage2() noexcept {
  current_string_buf_loc = doc.string_buf.get();
  current_loc = 0;
  valid = false;
  error = UNINITIALIZED;
}

really_inline error_code parser::on_error(error_code new_error_code) noexcept {
  error = new_error_code;
  return new_error_code;
}
really_inline error_code parser::on_success(error_code success_code) noexcept {
  error = success_code;
  valid = true;
  return success_code;
}
// increment_count increments the count of keys in an object or values in an array.
// Note that if you are at the level of the values or elements, the count
// must be increment in the preceding depth (depth-1) where the array or
// the object resides.
really_inline void parser::increment_count(uint32_t depth) noexcept {
  containing_scope[depth].count++;
}

really_inline bool parser::on_start_document(uint32_t depth) noexcept {
  containing_scope[depth].tape_index = current_loc;
  containing_scope[depth].count = 0;
  write_tape(0, internal::tape_type::ROOT); // if the document is correct, this gets rewritten later
  return true;
}
really_inline bool parser::on_start_object(uint32_t depth) noexcept {
  containing_scope[depth].tape_index = current_loc;
  containing_scope[depth].count = 0;
  write_tape(0, internal::tape_type::START_OBJECT);  // if the document is correct, this gets rewritten later
  return true;
}
really_inline bool parser::on_start_array(uint32_t depth) noexcept {
  containing_scope[depth].tape_index = current_loc;
  containing_scope[depth].count = 0;
  write_tape(0, internal::tape_type::START_ARRAY);  // if the document is correct, this gets rewritten later
  return true;
}
// TODO we're not checking this bool
really_inline bool parser::on_end_document(uint32_t depth) noexcept {
  // write our doc.tape location to the header scope
  // The root scope gets written *at* the previous location.
  write_tape(containing_scope[depth].tape_index, internal::tape_type::ROOT);
  end_scope(depth);
  return true;
}
really_inline bool parser::on_end_object(uint32_t depth) noexcept {
  // write our doc.tape location to the header scope
  write_tape(containing_scope[depth].tape_index, internal::tape_type::END_OBJECT);
  end_scope(depth);
  return true;
}
really_inline bool parser::on_end_array(uint32_t depth) noexcept {
  // write our doc.tape location to the header scope
  write_tape(containing_scope[depth].tape_index, internal::tape_type::END_ARRAY);
  end_scope(depth);
  return true;
}

really_inline bool parser::on_true_atom() noexcept {
  write_tape(0, internal::tape_type::TRUE_VALUE);
  return true;
}
really_inline bool parser::on_false_atom() noexcept {
  write_tape(0, internal::tape_type::FALSE_VALUE);
  return true;
}
really_inline bool parser::on_null_atom() noexcept {
  write_tape(0, internal::tape_type::NULL_VALUE);
  return true;
}

really_inline uint8_t *parser::on_start_string() noexcept {
  /* We advance the pointer. */
  // If we limit JSON documents to strictly less 4GB of
  // string content, then current_string_buf_loc
  // - doc.string_buf.get() fits in 32 bits. This leaves us
  // three free bytes.
  uint32_t position = uint32_t(current_string_buf_loc - doc.string_buf.get()); // cannot overflow because documents are limited to < 4GB
  write_tape(uint64_t(position), internal::tape_type::STRING);
  return current_string_buf_loc;
}

really_inline bool parser::on_end_string(uint8_t *dst) noexcept {
  uint32_t str_length = uint32_t(dst - current_string_buf_loc);
  // Long document support: Currently, simdjson supports only document
  // up to 4GB.
  // Should we change this constraint, we should then check for overflow in case
  // someone has a crazy string (>=4GB?).

  // We have two scenarios here. Either the string length is
  // less than 0x7fffff in which case, we have room in the string
  // header and all is good. Otherwise, we can encode the
  // string length in the document itself, taking care to
  // ensure that we do so in ASCII.
  if(likely(str_length <= 0x7fffff)) { // likely
    doc.tape[current_loc-1] |=  uint64_t(str_length) << 32;
  } else {
    // Oh gosh, we have a long string (8MB). We expect that this is
    // highly uncommon. We want to keep everything else super efficient,
    // so we will pay a complexity price for this one uncommon case.
    int offset = 2;
    uint64_t payload = doc.tape[current_loc-1] & 0xFFFFFFFF;
    payload += offset;
    payload |=  uint64_t(0x800000 | (str_length >> 9)) << 32;
    doc.tape[current_loc-1] =  payload  | ((uint64_t(char(internal::tape_type::STRING))) << 56);
    // We are going to make room. This copy is not free. However,
    // it allows us to handle the common case with ease and with
    // relatively little complexity. And a memcopy is not that slow:
    // it may run at tens of GB/s.
    // And we expect that it will effectively never happen in practice
    // so there is no cause to complexify the rest of the code.
    memmove(current_string_buf_loc + offset, current_string_buf_loc, str_length);
    dst += offset;
    // We have three free bytes, but
    // we need a leading 1, so that's 24-1 = 23. 32-23=9 remaining bits.
    // We have 9 bits left to code, which we do on the string buffer
    // using two bytes. We encoding the binary data using ASCII characters.
    // See https://lemire.me/blog/2020/05/02/encoding-binary-in-ascii-very-fast/
    // for a more general approach.
    //
    // These two bytes will appear right before where the string is.
    current_string_buf_loc[0] = uint8_t(32 + ((str_length & 0x1f0) >> 4)); // (0x1f0>>4)+32 = 63
    current_string_buf_loc[1] = uint8_t(32 + (str_length & 0xf)); // 32 + 0xf = 47
  }
  // NULL termination is still handy if you expect all your strings to
  // be NULL terminated? It comes at a small cost.
  *dst = 0;
  current_string_buf_loc = dst + 1;
  return true;
}

really_inline bool parser::on_number_s64(int64_t value) noexcept {
  write_tape(0, internal::tape_type::INT64);
  std::memcpy(&doc.tape[current_loc], &value, sizeof(value));
  ++current_loc;
  return true;
}
really_inline bool parser::on_number_u64(uint64_t value) noexcept {
  write_tape(0, internal::tape_type::UINT64);
  doc.tape[current_loc++] = value;
  return true;
}
really_inline bool parser::on_number_double(double value) noexcept {
  write_tape(0, internal::tape_type::DOUBLE);
  static_assert(sizeof(value) == sizeof(doc.tape[current_loc]), "mismatch size");
  memcpy(&doc.tape[current_loc++], &value, sizeof(double));
  // doc.tape[doc.current_loc++] = *((uint64_t *)&d);
  return true;
}

really_inline void parser::write_tape(uint64_t val, internal::tape_type t) noexcept {
  doc.tape[current_loc++] = val | ((uint64_t(char(t))) << 56);
}

// this function is responsible for annotating the start of the scope
really_inline void parser::end_scope(uint32_t depth) noexcept {
  scope_descriptor d = containing_scope[depth];
  // count can overflow if it exceeds 24 bits... so we saturate
  // the convention being that a cnt of 0xffffff or more is undetermined in value (>=  0xffffff).
  const uint32_t cntsat =  d.count > 0xFFFFFF ? 0xFFFFFF : d.count;
  // This is a load and an OR. It would be possible to just write once at doc.tape[d.tape_index]
  doc.tape[d.tape_index] |= current_loc | (uint64_t(cntsat) << 32);
}

} // namespace simdjson
} // namespace dom

#endif // SIMDJSON_DOCUMENT_PARSER_CALLBACKS_H
