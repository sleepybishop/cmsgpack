#ifndef MSGPACK_H
#define MSGPACK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mp_type {
  MP_NIL = 0,
  MP_BOOL,
  MP_FLT,
  MP_INT,
  MP_STR,
  MP_BLOB,
  MP_EXT,
  MP_ARR,
  MP_MAP,
};

typedef struct mp_node {
  struct mp_node *next;
  struct mp_node *prev;
  struct mp_node *child;
  struct mp_node *key;
  enum mp_type type;
  uint8_t etype;

  unsigned char *data;

  union {
    int64_t intval;
    uint64_t uintval;
    double doubleval;
  } number;
} mp_node;

int msgpack_unpack(mp_node **L, const void *ptr, size_t len);
void *msgpack_pack(mp_node *L, size_t *len);
void msgpack_free(mp_node *c);

size_t msgpack_get_array_size(mp_node *L);
mp_node *msgpack_get_array_item(mp_node *L, int idx);
mp_node *msgpack_get_map_item(mp_node *map, const char *name);

mp_node *msgpack_create_nil(void);
mp_node *msgpack_create_true(void);
mp_node *msgpack_create_false(void);
mp_node *msgpack_create_bool(bool b);
mp_node *msgpack_create_integer(int64_t num);
mp_node *msgpack_create_number(double num);
mp_node *msgpack_create_string(const char *string);
mp_node *msgpack_create_binary(const void *blob, size_t len);
mp_node *msgpack_create_array(void);
mp_node *msgpack_create_map(void);

void msgpack_add_item_to_array(mp_node *L, mp_node *item);
void msgpack_add_item_to_map(mp_node *map, const char *name, mp_node *item);

mp_node *msgpack_detach_item_from_array(mp_node *L, int idx);
void msgpack_delete_item_from_array(mp_node *L, int idx);
mp_node *msgpack_detach_item_from_map(mp_node *map, const char *name);
void msgpack_delete_item_from_map(mp_node *map, const char *name);

void msgpack_replace_item_in_array(mp_node *L, int idx, mp_node *newitem);
void msgpack_replace_item_in_map(mp_node *map, const char *name,
                                 mp_node *newitem);

mp_node *msgpack_duplicate(mp_node *item, int recurse);

#define msgpack_add_nil_to_map(map, name)                                      \
  msgpack_add_item_to_map(map, name, msgpack_create_nil())
#define msgpack_add_true_to_map(map, name)                                     \
  msgpack_add_item_to_map(map, name, msgpack_create_bool(1))
#define msgpack_add_false_to_map(map, name)                                    \
  msgpack_add_item_to_map(map, name, msgpack_create_bool(0))
#define msgpack_add_bool_to_map(map, name, b)                                  \
  msgpack_add_item_to_map(map, name, msgpack_create_bool(b))
#define msgpack_add_integer_to_map(map, name, n)                               \
  msgpack_add_item_to_map(map, name, msgpack_create_integer(n))
#define msgpack_add_number_to_map(map, name, n)                                \
  msgpack_add_item_to_map(map, name, msgpack_create_number(n))
#define msgpack_add_string_to_map(map, name, s)                                \
  msgpack_add_item_to_map(map, name, msgpack_create_string(s))

#ifdef __cplusplus
}
#endif /* MSGPACK_H */

#endif
