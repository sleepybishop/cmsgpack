#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msgpack.h"

#include "lua_cmsgpack.c"

void msgpack_free(mp_node *c) {
  mp_node *next;
  while (c) {
    next = c->next;
    if (c->key)
      msgpack_free(c->key);
    if (c->child)
      msgpack_free(c->child);
    if (c->data)
      free(c->data);
    free(c);
    c = next;
  }
}

int msgpack_unpack(mp_node **L, const void *ptr, size_t len) {
  int ret = 0;
  *L = mp_node_new();
  ret = mp_unpack(*L, ptr, len);

  if (ret <= 0) {
    msgpack_free(*L);
  }

  return ret;
}

size_t msgpack_get_array_size(mp_node *L) {
  mp_node *c = L->child;
  int i = 0;
  while (c)
    i++, c = c->next;
  return i;
}

mp_node *msgpack_get_array_item(mp_node *L, int idx) {
  mp_node *c = L->child;
  while (c && idx > 0)
    idx--, c = c->next;
  return c;
}

mp_node *msgpack_get_map_item(mp_node *map, const char *name) {
  mp_node *c = map->child;
  while (c && c->key && (c->key->type == MP_STR &&
         strcasecmp((char *)c->key->data, name) != 0))
    c = c->next;
  if ((c == NULL) || (c->key == NULL || c->key->data == NULL)) 
    return NULL;
  return c;
}

mp_node *msgpack_create_nil(void) {
  mp_node *item = mp_node_new();
  if (item)
    item->type = MP_NIL;
  return item;
}

mp_node *msgpack_create_true(void) {
  mp_node *item = mp_node_new();
  if (item) {
    item->type = MP_BOOL;
    item->number.intval = 1;
  }
  return item;
}

mp_node *msgpack_create_false(void) {
  mp_node *item = mp_node_new();
  if (item) {
    item->type = MP_BOOL;
    item->number.intval = 0;
  }
  return item;
}

mp_node *msgpack_create_bool(bool b) {
  mp_node *item = mp_node_new();
  if (item) {
    item->type = MP_BOOL;
    item->number.intval = b ? 1 : 0;
  }
  return item;
}

mp_node *msgpack_create_integer(int64_t num) {
  mp_node *item = mp_node_new();
  if (item) {
    item->type = MP_INT;
    item->number.intval = num;
  }
  return item;
}

mp_node *msgpack_create_number(double num) {
  mp_node *item = mp_node_new();
  if (item) {
    item->type = MP_FLT;
    item->number.doubleval = num;
  }
  return item;
}

mp_node *msgpack_create_string(const char *string) {
  mp_node *item = mp_node_new();
  if (item) {
    mp_push_data(item, string, strlen(string), MP_STR);
  }
  return item;
}

mp_node *msgpack_create_blob(const void *blob, size_t len) {
  mp_node *item = mp_node_new();
  if (item) {
    mp_push_data(item, blob, len, MP_BLOB);
  }
  return item;
}

mp_node *msgpack_create_array(void) {
  mp_node *item = mp_node_new();
  if (item)
    item->type = MP_ARR;
  return item;
}

mp_node *msgpack_create_map(void) {
  mp_node *item = mp_node_new();
  if (item)
    item->type = MP_MAP;
  return item;
}

void mp_encode_value(mp_buf *buf, mp_node *L) {
  if (!L)
    return;

  switch (L->type) {
  case MP_NIL: {
    unsigned char b[1] = {0xc0};
    mp_buf_append(buf, b, 1);
  } break;
  case MP_BOOL: {
    unsigned char b[1];
    b[0] = L->number.intval ? 0xc3 : 0xc2;
    mp_buf_append(buf, b, 1);
  } break;
  case MP_INT: {
    mp_encode_int(buf, L->number.intval);
  } break;
  case MP_FLT: {
    mp_encode_double(buf, L->number.doubleval);
  } break;
  case MP_BLOB:
    mp_encode_blob(buf, L->data, L->number.intval);
    break;
  case MP_STR:
    mp_encode_bytes(buf, L->data, L->number.intval);
    break;
  case MP_ARR: {
    size_t sz = msgpack_get_array_size(L);
    mp_encode_array(buf, sz);
    mp_node *n = L->child;
    while (n) {
      mp_encode_value(buf, n);
      n = n->next;
    }
  } break;
  case MP_MAP: {
    size_t sz = msgpack_get_array_size(L);
    mp_encode_map(buf, sz);
    mp_node *n = L->child;
    while (n) {
      mp_encode_value(buf, n->key);
      mp_encode_value(buf, n);
      n = n->next;
    }
  } break;
  case MP_EXT:
    mp_encode_ext(buf, L->etype, L->data, L->number.intval);
    break;
  }
}

void *msgpack_pack(mp_node *L, size_t *len) {
  mp_buf *buf = NULL;
  void *ret = NULL;

  buf = mp_buf_new();
  mp_encode_value(buf, L);

  if (buf->len > 0) {
    *len = buf->len;
    ret = buf->b;
    buf->b = NULL;
  }

  mp_buf_free(buf);
  return ret;
}

void msgpack_add_item_to_array(mp_node *L, mp_node *item) {
  mp_node *c = L->child;
  if (!item)
    return;
  if (!c) {
    L->child = item;
  } else {
    while (c && c->next)
      c = c->next;

    c->next = item;
    item->prev = c;
  }
}

void msgpack_add_item_to_map(mp_node *map, const char *name, mp_node *item) {
  if (!item)
    return;
  item->key = msgpack_create_string(name);
  msgpack_add_item_to_array(map, item);
}

mp_node *msgpack_detach_item_from_array(mp_node *L, int idx) {
  mp_node *c = L->child;
  while (c && idx > 0)
    c = c->next, idx--;
  if (!c)
    return 0;
  if (c->prev)
    c->prev->next = c->next;
  if (c->next)
    c->next->prev = c->prev;
  if (c == L->child)
    L->child = c->next;
  c->prev = c->next = 0;
  return c;
}

void msgpack_delete_item_from_array(mp_node *L, int idx) {
  msgpack_free(msgpack_detach_item_from_array(L, idx));
}

mp_node *msgpack_detach_item_from_map(mp_node *L, const char *name) {
  int i = 0;
  mp_node *c = L->child;
  while (c && c->key && c->key->type != MP_STR &&
         strcasecmp((char *)c->key->data, name))
    i++, c = c->next;
  if (c)
    return msgpack_detach_item_from_array(L, i);
  return 0;
}

void msgpack_delete_item_from_map(mp_node *L, const char *name) {
  msgpack_free(msgpack_detach_item_from_map(L, name));
}

void msgpack_replace_item_in_array(mp_node *L, int idx, mp_node *newitem) {
  mp_node *c = L->child;
  while (c && idx > 0)
    c = c->next, idx--;
  if (!c)
    return;
  newitem->next = c->next;
  newitem->prev = c->prev;
  if (newitem->next)
    newitem->next->prev = newitem;
  if (c == L->child)
    L->child = newitem;
  else
    newitem->prev->next = newitem;
  c->next = c->prev = 0;
  msgpack_free(c);
}

void msgpack_replace_item_in_map(mp_node *L, const char *name,
                                 mp_node *newitem) {
  int i = 0;
  mp_node *c = L->child;
  while (c && c->key && c->key->type != MP_STR &&
         strcasecmp((char *)c->key->data, name))

    i++, c = c->next;
  if (c) {
    newitem->key = msgpack_create_string(name);
    msgpack_replace_item_in_array(L, i, newitem);
  }
}

mp_node *msgpack_duplicate(mp_node *item, int recurse) {
  mp_node *newitem, *cptr, *nptr = 0, *newchild;
  /* Bail on bad ptr */
  if (!item)
    return 0;
  /* Create new item */
  newitem = mp_node_new();

  /* Copy over all vars */
  newitem->type = item->type;
  if (item->type == MP_INT) {
    newitem->number.intval = item->number.intval;
  } else if (item->type == MP_FLT) {
    newitem->number.doubleval = item->number.doubleval;
  }

  if (item->data) {
    mp_push_data(item, item->data, item->number.intval, item->type);
  }

  if (item->key) {
    newitem->key = msgpack_duplicate(item->key, 0);
    if (!newitem->key) {
      msgpack_free(newitem);
      return 0;
    }
  }
  /* If non-recursive, then we're done! */
  if (!recurse)
    return newitem;
  /* Walk the ->next chain for the child. */
  cptr = item->child;
  while (cptr) {
    newchild = msgpack_duplicate(cptr, 1);
    if (!newchild) {
      msgpack_free(newitem);
      return 0;
    }
    if (nptr) {
      nptr->next = newchild, newchild->prev = nptr;
      nptr = newchild;
    } /* If newitem->child already set, then crosswire ->prev and ->next and
         move on */
    else {
      newitem->child = newchild;
      nptr = newchild;
    } /* Set newitem->child and move to it */
    cptr = cptr->next;
  }
  return newitem;
}
