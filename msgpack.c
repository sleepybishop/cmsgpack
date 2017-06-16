#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msgpack.h"

/* -------------------------- Endian conversion --------------------------------
 * We use it only for floats and doubles, all the other conversions performed
 * in an endian independent fashion. So the only thing we need is a function
 * that swaps a binary string if arch is little endian (and left it untouched
 * otherwise). */

void memrevifle(void *ptr, size_t len) {
  unsigned char *p = (unsigned char *)ptr, *e = (unsigned char *)p + len - 1,
                aux;
  int test = 1;
  unsigned char *testp = (unsigned char *)&test;

  if (testp[0] == 0)
    return; /* Big endian, nothing to do. */
  len /= 2;
  while (len--) {
    aux = *p;
    *p = *e;
    *e = aux;
    p++;
    e--;
  }
}

/* ---------------------------- String buffer ----------------------------------
 * This is a simple implementation of string buffers. The only operation
 * supported is creating empty buffers and appending bytes to it.
 * The string buffer uses 2x preallocation on every realloc for O(N) append
 * behavior.  */

typedef struct mp_buf {
  unsigned char *b;
  size_t len, free;
} mp_buf;

void *mp_realloc(void *target, size_t osize, size_t nsize) {
  if (nsize == 0) {
    free(target);
    target = NULL;
  } else if (osize == 0) {
    target = calloc(1, nsize);
  } else {
    target = realloc(target, nsize);
  }
  return target;
}

mp_buf *mp_buf_new() {
  mp_buf *buf = NULL;

  /* Old size = 0; new size = sizeof(*buf) */
  buf = (mp_buf *)mp_realloc(NULL, 0, sizeof(*buf));

  buf->b = NULL;
  buf->len = buf->free = 0;
  return buf;
}

void mp_buf_append(mp_buf *buf, const unsigned char *s, size_t len) {
  if (buf->free < len) {
    size_t newsize = (buf->len + len) * 2;

    buf->b = (unsigned char *)mp_realloc(buf->b, buf->len + buf->free, newsize);
    buf->free = newsize - buf->len;
  }
  memcpy(buf->b + buf->len, s, len);
  buf->len += len;
  buf->free -= len;
}

void mp_buf_free(mp_buf *buf) {
  if (buf) {
    if (buf->b)
      mp_realloc(buf->b, buf->len + buf->free, 0); /* realloc to 0 = free */
    mp_realloc(buf, sizeof(*buf), 0);
  }
}

/* ---------------------------- String cursor ----------------------------------
 * This simple data structure is used for parsing. Basically you create a cursor
 * using a string pointer and a length, then it is possible to access the
 * current string position with cursor->p, check the remaining length
 * in cursor->left, and finally consume more string using
 * mp_cur_consume(cursor,len), to advance 'p' and subtract 'left'.
 * An additional field cursor->error is set to zero on initialization and can
 * be used to report errors. */

#define MP_CUR_ERROR_NONE 0
#define MP_CUR_ERROR_EOF 1    /* Not enough data to complete operation. */
#define MP_CUR_ERROR_BADFMT 2 /* Bad data format */

typedef struct mp_cur {
  const unsigned char *p;
  size_t left;
  int err;
} mp_cur;

void mp_cur_init(mp_cur *cursor, const unsigned char *s, size_t len) {
  cursor->p = s;
  cursor->left = len;
  cursor->err = MP_CUR_ERROR_NONE;
}

#define mp_cur_consume(_c, _len)                                               \
  do {                                                                         \
    _c->p += _len;                                                             \
    _c->left -= _len;                                                          \
  } while (0)

/* When there is not enough room we set an error in the cursor and return. This
 * is very common across the code so we have a macro to make the code look
 * a bit simpler. */
#define mp_cur_need(_c, _len)                                                  \
  do {                                                                         \
    if (_c->left < _len) {                                                     \
      _c->err = MP_CUR_ERROR_EOF;                                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

/* ------------------------- Low level MP encoding -------------------------- */

void mp_encode_bytes(mp_buf *buf, const unsigned char *s, size_t len) {
  unsigned char hdr[5];
  int hdrlen;

  if (len < 32) {
    hdr[0] = 0xa0 | (len & 0xff);
    hdrlen = 1;
  } else if (len <= 0xff) {
    hdr[0] = 0xd9;
    hdr[1] = len;
    hdrlen = 2;
  } else if (len <= 0xffff) {
    hdr[0] = 0xda;
    hdr[1] = (len & 0xff00) >> 8;
    hdr[2] = len & 0xff;
    hdrlen = 3;
  } else {
    hdr[0] = 0xdb;
    hdr[1] = (len & 0xff000000) >> 24;
    hdr[2] = (len & 0xff0000) >> 16;
    hdr[3] = (len & 0xff00) >> 8;
    hdr[4] = len & 0xff;
    hdrlen = 5;
  }
  mp_buf_append(buf, hdr, hdrlen);
  mp_buf_append(buf, s, len);
}

void mp_encode_blob(mp_buf *buf, const unsigned char *s, size_t len) {
  unsigned char hdr[5];
  int hdrlen;

  if (len <= 0xff) {
    hdr[0] = 0xc4;
    hdr[1] = len;
    hdrlen = 2;
  } else if (len <= 0xffff) {
    hdr[0] = 0xc5;
    hdr[1] = (len & 0xff00) >> 8;
    hdr[2] = len & 0xff;
    hdrlen = 3;
  } else {
    hdr[0] = 0xc6;
    hdr[1] = (len & 0xff000000) >> 24;
    hdr[2] = (len & 0xff0000) >> 16;
    hdr[3] = (len & 0xff00) >> 8;
    hdr[4] = len & 0xff;
    hdrlen = 5;
  }
  mp_buf_append(buf, hdr, hdrlen);
  mp_buf_append(buf, s, len);
}

void mp_encode_ext(mp_buf *buf, uint8_t etype, const unsigned char *s,
                   size_t len) {
  unsigned char hdr[6];
  int hdrlen;

  if (len <= 16 && __builtin_popcount(len) == 1) {
    hdr[0] = 0xd4 + __builtin_ctz(len); /* fixextN */
    hdr[1] = etype;
    hdrlen = 2;
  } else if (len <= 0xff) {
    hdr[0] = 0xc7;
    hdr[1] = etype;
    hdr[2] = len;
    hdrlen = 3;
  } else if (len <= 0xffff) {
    hdr[0] = 0xc8;
    hdr[1] = etype;
    hdr[2] = (len & 0xff00) >> 8;
    hdr[3] = len & 0xff;
    hdrlen = 4;
  } else {
    hdr[0] = 0xc9;
    hdr[1] = etype;
    hdr[2] = (len & 0xff000000) >> 24;
    hdr[3] = (len & 0xff0000) >> 16;
    hdr[4] = (len & 0xff00) >> 8;
    hdr[5] = len & 0xff;
    hdrlen = 6;
  }
  mp_buf_append(buf, hdr, hdrlen);
  mp_buf_append(buf, s, len);
}

/* we assume IEEE 754 internal format for single and double precision floats. */
void mp_encode_double(mp_buf *buf, double d) {
  unsigned char b[9];
  float f = d;

  assert(sizeof(f) == 4 && sizeof(d) == 8);
  if (d == (double)f) {
    b[0] = 0xca; /* float IEEE 754 */
    memcpy(b + 1, &f, 4);
    memrevifle(b + 1, 4);
    mp_buf_append(buf, b, 5);
  } else if (sizeof(d) == 8) {
    b[0] = 0xcb; /* double IEEE 754 */
    memcpy(b + 1, &d, 8);
    memrevifle(b + 1, 8);
    mp_buf_append(buf, b, 9);
  }
}

void mp_encode_int(mp_buf *buf, int64_t n) {
  unsigned char b[9];
  int enclen;

  if (n >= 0) {
    if (n <= 127) {
      b[0] = n & 0x7f; /* positive fixnum */
      enclen = 1;
    } else if (n <= 0xff) {
      b[0] = 0xcc; /* uint 8 */
      b[1] = n & 0xff;
      enclen = 2;
    } else if (n <= 0xffff) {
      b[0] = 0xcd; /* uint 16 */
      b[1] = (n & 0xff00) >> 8;
      b[2] = n & 0xff;
      enclen = 3;
    } else if (n <= 0xffffffffLL) {
      b[0] = 0xce; /* uint 32 */
      b[1] = (n & 0xff000000) >> 24;
      b[2] = (n & 0xff0000) >> 16;
      b[3] = (n & 0xff00) >> 8;
      b[4] = n & 0xff;
      enclen = 5;
    } else {
      b[0] = 0xcf; /* uint 64 */
      b[1] = (n & 0xff00000000000000LL) >> 56;
      b[2] = (n & 0xff000000000000LL) >> 48;
      b[3] = (n & 0xff0000000000LL) >> 40;
      b[4] = (n & 0xff00000000LL) >> 32;
      b[5] = (n & 0xff000000) >> 24;
      b[6] = (n & 0xff0000) >> 16;
      b[7] = (n & 0xff00) >> 8;
      b[8] = n & 0xff;
      enclen = 9;
    }
  } else {
    if (n >= -32) {
      b[0] = ((signed char)n); /* negative fixnum */
      enclen = 1;
    } else if (n >= -128) {
      b[0] = 0xd0; /* int 8 */
      b[1] = n & 0xff;
      enclen = 2;
    } else if (n >= -32768) {
      b[0] = 0xd1; /* int 16 */
      b[1] = (n & 0xff00) >> 8;
      b[2] = n & 0xff;
      enclen = 3;
    } else if (n >= -2147483648LL) {
      b[0] = 0xd2; /* int 32 */
      b[1] = (n & 0xff000000) >> 24;
      b[2] = (n & 0xff0000) >> 16;
      b[3] = (n & 0xff00) >> 8;
      b[4] = n & 0xff;
      enclen = 5;
    } else {
      b[0] = 0xd3; /* int 64 */
      b[1] = (n & 0xff00000000000000LL) >> 56;
      b[2] = (n & 0xff000000000000LL) >> 48;
      b[3] = (n & 0xff0000000000LL) >> 40;
      b[4] = (n & 0xff00000000LL) >> 32;
      b[5] = (n & 0xff000000) >> 24;
      b[6] = (n & 0xff0000) >> 16;
      b[7] = (n & 0xff00) >> 8;
      b[8] = n & 0xff;
      enclen = 9;
    }
  }
  mp_buf_append(buf, b, enclen);
}

void mp_encode_array(mp_buf *buf, int64_t n) {
  unsigned char b[5];
  int enclen;

  if (n <= 15) {
    b[0] = 0x90 | (n & 0xf); /* fix array */
    enclen = 1;
  } else if (n <= 65535) {
    b[0] = 0xdc; /* array 16 */
    b[1] = (n & 0xff00) >> 8;
    b[2] = n & 0xff;
    enclen = 3;
  } else {
    b[0] = 0xdd; /* array 32 */
    b[1] = (n & 0xff000000) >> 24;
    b[2] = (n & 0xff0000) >> 16;
    b[3] = (n & 0xff00) >> 8;
    b[4] = n & 0xff;
    enclen = 5;
  }
  mp_buf_append(buf, b, enclen);
}

void mp_encode_map(mp_buf *buf, int64_t n) {
  unsigned char b[5];
  int enclen;

  if (n <= 15) {
    b[0] = 0x80 | (n & 0xf); /* fix map */
    enclen = 1;
  } else if (n <= 65535) {
    b[0] = 0xde; /* map 16 */
    b[1] = (n & 0xff00) >> 8;
    b[2] = n & 0xff;
    enclen = 3;
  } else {
    b[0] = 0xdf; /* map 32 */
    b[1] = (n & 0xff000000) >> 24;
    b[2] = (n & 0xff0000) >> 16;
    b[3] = (n & 0xff00) >> 8;
    b[4] = n & 0xff;
    enclen = 5;
  }
  mp_buf_append(buf, b, enclen);
}

/* ------------------------------- Decoding --------------------------------- */
static void mp_decode_value(mp_node *L, mp_cur *c);

static mp_node *mp_node_new(void) {
  return (mp_node *)mp_realloc(NULL, 0, sizeof(mp_node));
}

static void mp_push_data(mp_node *L, const void *data, size_t count,
                         enum mp_type type) {
  unsigned char *new = NULL;

  new = (unsigned char *)mp_realloc(NULL, 0, count + 1);

  L->type = type;
  memcpy(new, data, count);
  L->data = new;
  L->number.intval = count;
}

static void mp_decode_array(mp_node *L, mp_cur *c, size_t len) {
  assert(len <= UINT_MAX);
  mp_node *child;

  L->type = MP_ARR;
  if (len == 0)
    return; /* empty */

  L->child = child = mp_node_new();
  mp_decode_value(child, c);
  if (c->err)
    return;
  len--;

  while (len--) {
    mp_node *new_item = mp_node_new();
    child->next = new_item;
    new_item->prev = child;
    child = new_item;
    mp_decode_value(child, c);
    if (c->err)
      return;
  }
}

static void mp_decode_hash(mp_node *L, mp_cur *c, size_t len) {
  assert(len <= UINT_MAX);
  mp_node *child, *key;

  L->type = MP_MAP;
  if (len == 0)
    return; /* empty */

  L->child = child = mp_node_new();
  L->child->key = key = mp_node_new();
  mp_decode_value(key, c);
  if (c->err)
    return;
  mp_decode_value(child, c);
  if (c->err)
    return;
  len--;

  while (len--) {
    mp_node *new_item = mp_node_new();
    mp_node *new_key = mp_node_new();
    child->next = new_item;
    new_item->prev = child;
    child = new_item;
    mp_decode_value(new_key, c);
    if (c->err)
      return;
    mp_decode_value(child, c);
    if (c->err)
      return;
    child->key = new_key;
  }
}

static void mp_decode_value(mp_node *L, mp_cur *c) {
  mp_cur_need(c, 1);

  switch (c->p[0]) {
  case 0xcc: /* uint 8 */
    mp_cur_need(c, 2);
    L->type = MP_INT;
    L->number.uintval = (uint8_t)c->p[1];

    mp_cur_consume(c, 2);
    break;
  case 0xd0: /* int 8 */
    mp_cur_need(c, 2);
    L->type = MP_INT;
    L->number.intval = (int8_t)c->p[1];

    mp_cur_consume(c, 2);
    break;
  case 0xcd: /* uint 16 */
    mp_cur_need(c, 3);
    L->type = MP_INT;
    L->number.uintval = (uint16_t)((c->p[1] << 8) | c->p[2]);
    mp_cur_consume(c, 3);
    break;
  case 0xd1: /* int 16 */
    mp_cur_need(c, 3);
    L->type = MP_INT;
    L->number.intval = (int16_t)((c->p[1] << 8) | c->p[2]);
    mp_cur_consume(c, 3);
    break;
  case 0xce: /* uint 32 */
    mp_cur_need(c, 5);
    L->type = MP_INT;
    L->number.uintval =
        (uint32_t)(((uint32_t)c->p[1] << 24) | ((uint32_t)c->p[2] << 16) |
                   ((uint32_t)c->p[3] << 8) | (uint32_t)c->p[4]);
    mp_cur_consume(c, 5);
    break;
  case 0xd2: /* int 32 */
    mp_cur_need(c, 5);
    L->type = MP_INT;
    L->number.intval =
        (int32_t)(((int32_t)c->p[1] << 24) | ((int32_t)c->p[2] << 16) |
                  ((int32_t)c->p[3] << 8) | (int32_t)c->p[4]);
    mp_cur_consume(c, 5);
    break;
  case 0xcf: /* uint 64 */
    mp_cur_need(c, 9);
    L->type = MP_INT;
    L->number.uintval =
        (uint64_t)(((uint64_t)c->p[1] << 56) | ((uint64_t)c->p[2] << 48) |
                   ((uint64_t)c->p[3] << 40) | ((uint64_t)c->p[4] << 32) |
                   ((uint64_t)c->p[5] << 24) | ((uint64_t)c->p[6] << 16) |
                   ((uint64_t)c->p[7] << 8) | (uint64_t)c->p[8]);
    mp_cur_consume(c, 9);
    break;
  case 0xd3: /* int 64 */
    mp_cur_need(c, 9);
    L->type = MP_INT;
    L->number.intval =
        (int64_t)(((int64_t)c->p[1] << 56) | ((int64_t)c->p[2] << 48) |
                  ((int64_t)c->p[3] << 40) | ((int64_t)c->p[4] << 32) |
                  ((int64_t)c->p[5] << 24) | ((int64_t)c->p[6] << 16) |
                  ((int64_t)c->p[7] << 8) | (int64_t)c->p[8]);
    mp_cur_consume(c, 9);
    break;
  case 0xc0: /* nil */
    L->type = MP_NIL;
    mp_cur_consume(c, 1);
    break;
  case 0xc2: /* false */
  case 0xc3: /* true */
    L->type = MP_BOOL;
    L->number.intval = c->p[0] - 0xc2;
    mp_cur_consume(c, 1);
    break;
  case 0xca: /* float */
    mp_cur_need(c, 5);
    assert(sizeof(float) == 4);
    {
      float f;
      memcpy(&f, c->p + 1, 4);
      memrevifle(&f, 4);
      L->type = MP_FLT;
      L->number.doubleval = f;
      mp_cur_consume(c, 5);
    }
    break;
  case 0xcb: /* double */
    mp_cur_need(c, 9);
    assert(sizeof(double) == 8);
    {
      double d;
      memcpy(&d, c->p + 1, 8);
      memrevifle(&d, 8);
      L->type = MP_FLT;
      L->number.doubleval = d;
      mp_cur_consume(c, 9);
    }
    break;
  case 0xd4: /* fixext1 */
  case 0xd5: /* fixext2 */
  case 0xd6: /* fixext4 */
  case 0xd7: /* fixext8 */
  case 0xd8: /* fixext16 */
    mp_cur_need(c, 2);
    {
      size_t l = 1 << (c->p[0] - 0xd4);
      L->etype = c->p[1];
      mp_cur_need(c, 2 + l);
      mp_push_data(L, (char *)c->p + 2, l, MP_EXT);
      mp_cur_consume(c, 2 + l);
    }
    break;
  case 0xc4: /* bin 8 */
  case 0xd9: /* str 8 */
    mp_cur_need(c, 2);
    {
      size_t l = c->p[1];
      mp_cur_need(c, 2 + l);
      mp_push_data(L, (char *)c->p + 2, l, (c->p[0] & 0x10) ? MP_STR : MP_BLOB);
      mp_cur_consume(c, 2 + l);
    }
    break;
  case 0xc5: /* bin 16 */
  case 0xda: /* str 16 */
    mp_cur_need(c, 3);
    {
      size_t l = (c->p[1] << 8) | c->p[2];
      mp_cur_need(c, 3 + l);
      mp_push_data(L, (char *)c->p + 3, l, (c->p[0] & 0x10) ? MP_STR : MP_BLOB);
      mp_cur_consume(c, 3 + l);
    }
    break;
  case 0xc6: /* bin 32 */
  case 0xdb: /* str 32 */
    mp_cur_need(c, 5);
    {
      size_t l = ((size_t)c->p[1] << 24) | ((size_t)c->p[2] << 16) |
                 ((size_t)c->p[3] << 8) | (size_t)c->p[4];
      mp_cur_consume(c, 5);
      mp_cur_need(c, l);
      mp_push_data(L, (char *)c->p, l, (c->p[0] & 0x10) ? MP_STR : MP_BLOB);
      mp_cur_consume(c, l);
    }
    break;
  case 0xc7: /* ext 8 */
    mp_cur_need(c, 3);
    {
      size_t l = c->p[2];
      L->etype = c->p[1];
      mp_cur_need(c, 3 + l);
      mp_push_data(L, (char *)c->p + 3, l, MP_EXT);
      mp_cur_consume(c, 3 + l);
    }
    break;
  case 0xc8: /* ext 16 */
    mp_cur_need(c, 4);
    {
      size_t l = (c->p[2] << 8) | c->p[3];
      L->etype = c->p[1];
      mp_cur_need(c, 4 + l);
      mp_push_data(L, (char *)c->p + 4, l, MP_EXT);
      mp_cur_consume(c, 4 + l);
    }
    break;
  case 0xc9: /* ext 32 */
    mp_cur_need(c, 6);
    {
      size_t l = ((size_t)c->p[1] << 24) | ((size_t)c->p[2] << 16) |
                 ((size_t)c->p[3] << 8) | (size_t)c->p[4];
      L->etype = c->p[1];
      mp_cur_consume(c, 6);
      mp_cur_need(c, l);
      mp_push_data(L, (char *)c->p, l, MP_EXT);
      mp_cur_consume(c, l);
    }
    break;
  case 0xdc: /* array 16 */
    mp_cur_need(c, 3);
    {
      size_t l = (c->p[1] << 8) | c->p[2];
      mp_cur_consume(c, 3);
      mp_decode_array(L, c, l);
    }
    break;
  case 0xdd: /* array 32 */
    mp_cur_need(c, 5);
    {
      size_t l = ((size_t)c->p[1] << 24) | ((size_t)c->p[2] << 16) |
                 ((size_t)c->p[3] << 8) | (size_t)c->p[4];
      mp_cur_consume(c, 5);
      mp_decode_array(L, c, l);
    }
    break;
  case 0xde: /* map 16 */
    mp_cur_need(c, 3);
    {
      size_t l = (c->p[1] << 8) | c->p[2];
      mp_cur_consume(c, 3);
      mp_decode_hash(L, c, l);
    }
    break;
  case 0xdf: /* map 32 */
    mp_cur_need(c, 5);
    {
      size_t l = ((size_t)c->p[1] << 24) | ((size_t)c->p[2] << 16) |
                 ((size_t)c->p[3] << 8) | (size_t)c->p[4];
      mp_cur_consume(c, 5);
      mp_decode_hash(L, c, l);
    }
    break;
  default: /* types that can't be idenitified by first byte value. */
    if ((c->p[0] & 0x80) == 0) { /* positive fixnum */
      L->type = MP_INT;
      L->number.intval = (uint8_t)c->p[0];
      mp_cur_consume(c, 1);
    } else if ((c->p[0] & 0xe0) == 0xe0) { /* negative fixnum */
      L->type = MP_INT;
      L->number.intval = (int8_t)c->p[0];
      mp_cur_consume(c, 1);
    } else if ((c->p[0] & 0xe0) == 0xa0) { /* fix str */
      size_t l = c->p[0] & 0x1f;
      mp_cur_need(c, 1 + l);
      mp_push_data(L, (char *)c->p + 1, l, MP_STR);
      mp_cur_consume(c, 1 + l);
    } else if ((c->p[0] & 0xf0) == 0x90) { /* fix map */
      size_t l = c->p[0] & 0xf;
      mp_cur_consume(c, 1);
      mp_decode_array(L, c, l);
    } else if ((c->p[0] & 0xf0) == 0x80) { /* fix map */
      size_t l = c->p[0] & 0xf;
      mp_cur_consume(c, 1);
      mp_decode_hash(L, c, l);
    } else {
      c->err = MP_CUR_ERROR_BADFMT;
    }
  }
}

static int mp_unpack(mp_node *L, const char *s, size_t len) {
  mp_cur c;
  int cnt; /* Number of objects unpacked */

  mp_cur_init(&c, (const unsigned char *)s, len);

  /* We loop over the decode because this could be a stream
   * of multiple top-level values serialized together */
  for (cnt = 0; c.left > 0; cnt++) {
    mp_decode_value(L, &c);

    if (c.err == MP_CUR_ERROR_EOF) {
      fprintf(stderr, "Missing bytes in input.");
      return -c.err;
    } else if (c.err == MP_CUR_ERROR_BADFMT) {
      fprintf(stderr, "Bad data format in input.");
      return -c.err;
    }

    if (c.left > 0) {
      L->next = mp_node_new();
      L = L->next;
    }
  }
  return cnt;
}

/* -------------------------- wrapper ----------------------------- */
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
  while (c && c->key && c->key->type != MP_STR &&
         strcasecmp((char *)c->key->data, name))
    c = c->next;
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
