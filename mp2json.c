#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "msgpack.h"
#include "hexcodec.h"

static cJSON *to_json(mp_node *item, int recurse) {
  cJSON *newitem, *newchild, *nptr = 0;
  mp_node *cptr;

  if (!item)
    return 0;

  newitem = cJSON_CreateNull();
  if (!newitem)
    return 0;

  switch (item->type) {
  case MP_NIL:
    break;
  case MP_BOOL: {
    newitem->type = (item->number.intval > 0) ? cJSON_True : cJSON_False;
    break;
  }
  case MP_FLT: {
    newitem->type = cJSON_Number;
    cJSON_SetIntValue(newitem, item->number.doubleval);
    break;
  }
  case MP_INT: {
    newitem->type = cJSON_Number;
    cJSON_SetIntValue(newitem, item->number.intval);
    break;
  }
  case MP_EXT: {
    newitem->type = cJSON_Object;
    cJSON_AddNumberToObject(newitem, "etype", item->etype);
    cJSON *data = cJSON_CreateNull();
    data->type = cJSON_String;
    data->valuestring = calloc(2, item->number.intval + 1);
    hex_encode((unsigned char *)data->valuestring, item->data,
               item->number.intval);
    cJSON_AddItemToObject(newitem, "data", data);
    break;
  }
  case MP_BLOB: {
    newitem->type = cJSON_String;
    newitem->valuestring = calloc(2, item->number.intval + 1);
    hex_encode((unsigned char *)newitem->valuestring, item->data,
               item->number.intval);
    break;
  }
  case MP_STR: {
    newitem->type = cJSON_String;
    newitem->valuestring = calloc(1, item->number.intval + 1);
    memcpy(newitem->valuestring, item->data, item->number.intval);
    break;
  }
  case MP_ARR: {
    newitem->type = cJSON_Array;
    break;
  }
  case MP_MAP: {
    newitem->type = cJSON_Object;
    break;
  }
  }

  if (item->key) {
    newitem->string = strdup((char *)item->key->data);
  }

  if (!recurse)
    return newitem;

  cptr = item->child;
  while (cptr) {
    newchild = to_json(cptr, 1);
    if (!newchild) {
      cJSON_Delete(newitem);
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
  mp_node *mp = NULL;
  FILE *st = open_memstream(&buf, &len);

  while (!feof(stdin)) {
    len += fread(tmp, 1, sizeof(tmp), stdin);
    fwrite(tmp, 1, len, st);
  }
  fclose(st);

  int cnt = msgpack_unpack(&mp, buf, len);
  if (cnt > 1) {
    mp_node *root = msgpack_create_array();
    root->child = mp;
    mp = root;
  }
  if (mp) {
    cJSON *k = to_json(mp, 1);
    if (k) {
      char *p = cJSON_Print(k);
      fprintf(stdout, "%s\n", p);
      free(p);
      cJSON_Delete(k);
    }
    msgpack_free(mp);
  }
  if (buf)
    free(buf);

  return 0;
}
