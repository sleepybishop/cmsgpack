/* =============================================================================
 * MessagePack implementation and bindings for Lua 5.1/5.2.
 * Copyright(C) 2012 Salvatore Sanfilippo <antirez@gmail.com>
 *
 * http://github.com/antirez/lua-cmsgpack
 *
 * For MessagePack specification check the following web site:
 * http://wiki.msgpack.org/display/MSGPACK/Format+specification
 *
 * See Copyright Notice at the end of this file.
 *
 * ========================================================================== */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------- Endian conversion --------------------------------
 * We use it only for floats and doubles, all the other conversions performed
 * in an endian independent fashion. So the only thing we need is a function
 * that swaps a binary string if arch is little endian (and left it untouched
 * otherwise). */

/* Reverse memory bytes if arch is little endian. Given the conceptual
 * simplicity of the Lua build system we prefer check for endianess at runtime.
 * The performance difference should be acceptable. */
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
    assert(buf->b != NULL);
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
/******************************************************************************
 * Copyright (C) 2012 Salvatore Sanfilippo.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ******************************************************************************/
