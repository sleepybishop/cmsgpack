#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <float.h>
#include <limits.h>
#include <math.h>

#include "cJSON.h"
#include "msgpack.h"

static mp_node *to_mp(cJSON *item, int recurse) {
  mp_node *newitem, *newchild, *nptr = 0;
  cJSON *cptr;

  if (!item)
    return 0;

  newitem = msgpack_create_nil();

  switch (item->type & (~cJSON_IsReference)) {
  case cJSON_NULL: {
    newitem->type = MP_NIL;
    break;
  }
  case cJSON_False: {
    newitem->type = MP_BOOL;
    newitem->number.intval = 0;
    break;
  }
  case cJSON_True: {
    newitem->type = MP_BOOL;
    newitem->number.intval = 1;
    break;
  }
  case cJSON_Number: {
    double d = item->valuedouble;
    if (fabs(((double)item->valueint) - d) <= DBL_EPSILON && d <= INT_MAX &&
        d >= INT_MIN) {
      newitem->type = MP_INT;
      newitem->number.intval = item->valueint;
    } else {
      newitem->type = MP_FLT;
      newitem->number.doubleval = item->valuedouble;
    }
    break;
  }
  case cJSON_String: {
    newitem->type = MP_STR;
    newitem->data = (unsigned char *)strdup(item->valuestring);
    newitem->number.intval = strlen(item->valuestring);
    break;
  }
  case cJSON_Array: {
    newitem->type = MP_ARR;
    break;
  }
  case cJSON_Object: {
    newitem->type = MP_MAP;
    break;
  }
  }

  if (item->string) {
    newitem->key = msgpack_create_string(item->string);
  }

  if (!recurse)
    return newitem;

  cptr = item->child;
  while (cptr) {
    newchild = to_mp(cptr, 1);
    if (!newchild) {
      msgpack_free(newitem);
      return 0;
    }
    if (nptr) {
      nptr->next = newchild, newchild->prev = nptr;
      nptr = newchild;
    } else {
      newitem->child = newchild;
      nptr = newchild;
    }
    cptr = cptr->next;
  }
  return newitem;
}

int main(int argc, char *argv[]) {
  char *buf = NULL, tmp[4096];
  size_t len = 0;
  cJSON *json = NULL;
  FILE *st = open_memstream(&buf, &len);

  while (!feof(stdin)) {
    len += fread(tmp, 1, sizeof(tmp), stdin);
    fwrite(tmp, 1, len, st);
  }
  fclose(st);

  json = cJSON_Parse(buf);
  if (json) {
    mp_node *mp = to_mp(json, 1);
    if (mp) {
      size_t len = 0;
      void *buf = msgpack_pack(mp, &len);
      if (buf) {
        fwrite(buf, 1, len, stdout);
        free(buf);
      }
      msgpack_free(mp);
    }
    cJSON_Delete(json);
  }
  if (buf)
    free(buf);

  return 0;
}
