/* Copyright 2004 Roger Dingledine */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

void rend_service_descriptor_free(rend_service_descriptor_t *desc)
{
  int i;
  if (desc->pk)
    crypto_free_pk_env(desc->pk);
  if (desc->intro_points) {
    for (i=0; i < desc->n_intro_points; ++i) {
      tor_free(desc->intro_points[i]);
    }
    tor_free(desc->intro_points);
  }
  tor_free(desc);
}

int
rend_encode_service_descriptor(rend_service_descriptor_t *desc,
                               crypto_pk_env_t *key,
                               char **str_out, int *len_out)
{
  char *buf, *cp, *ipoint;
  int i, keylen, asn1len;
  keylen = crypto_pk_keysize(desc->pk);
  buf = tor_malloc(keylen*2); /* XXXX */
  asn1len = crypto_pk_asn1_encode(desc->pk, buf, keylen*2);
  if (asn1len<0) {
    tor_free(buf);
    return -1;
  }
  *len_out = 2 + asn1len + 4 + 2 + keylen;
  for (i = 0; i < desc->n_intro_points; ++i) {
    *len_out += strlen(desc->intro_points[i]) + 1;
  }
  cp = *str_out = tor_malloc(*len_out);
  set_uint16(cp, (uint16_t)asn1len);
  cp += 2;
  memcpy(cp, buf, asn1len);
  tor_free(buf);
  cp += asn1len;
  set_uint32(cp, (uint32_t)desc->timestamp);
  cp += 4;
  set_uint16(cp, (uint16_t)desc->n_intro_points);
  cp += 2;
  for (i=0; i < desc->n_intro_points; ++i) {
    ipoint = (char*)desc->intro_points[i];
    strcpy(cp, ipoint);
    cp += strlen(ipoint)+1;
  }
  i = crypto_pk_private_sign_digest(key, *str_out, cp-*str_out, cp);
  if (i<0) {
    tor_free(*str_out);
    return -1;
  }
  cp += i;
  assert(*len_out == (cp-*str_out));
  return 0;
}

rend_service_descriptor_t *rend_parse_service_descriptor(
                           const char *str, int len)
{
  rend_service_descriptor_t *result = NULL;
  int keylen, asn1len, i;
  const char *end, *cp, *eos;

  result = tor_malloc_zero(sizeof(rend_service_descriptor_t));
  cp = str;
  end = str+len;
  if (end-cp < 2) goto truncated;
  asn1len = get_uint16(cp);
  cp += 2;
  if (end-cp < asn1len) goto truncated;
  result->pk = crypto_pk_asn1_decode(cp, asn1len);
  if (!result->pk) goto truncated;
  cp += asn1len;
  if (end-cp < 4) goto truncated;
  result->timestamp = (time_t) get_uint32(cp);
  cp += 4;
  if (end-cp < 2) goto truncated;
  result->n_intro_points = get_uint16(cp);
  result->intro_points = tor_malloc_zero(sizeof(char*)*result->n_intro_points);
  cp += 2;
  for (i=0;i<result->n_intro_points;++i) {
    if (end-cp < 2) goto truncated;
    eos = (const char *)memchr(cp,'\0',end-cp);
    if (!eos) goto truncated;
    result->intro_points[i] = tor_strdup(cp);
    cp = eos+1;
  }
  keylen = crypto_pk_keysize(result->pk);
  if (end-cp < keylen) goto truncated;
  if (end-cp > keylen) {
    log_fn(LOG_WARN, "Signature too long on service descriptor");
    goto error;
  }
  if (crypto_pk_public_checksig_digest(result->pk,
                                       (char*)str,cp-str, /* data */
                                       (char*)cp,end-cp  /* signature*/
                                       )<0) {
    log_fn(LOG_WARN, "Bad signature on service descriptor");
    goto error;
  }

  return result;
 truncated:
  log_fn(LOG_WARN, "Truncated service descriptor");
 error:
  rend_service_descriptor_free(result);
  return NULL;
}

int rend_get_service_id(crypto_pk_env_t *pk, char *out)
{
  char buf[DIGEST_LEN];
  assert(pk);
  if (crypto_pk_get_digest(pk, buf) < 0)
    return -1;
  if (base32_encode(out, REND_SERVICE_ID_LEN+1, buf, 10) < 0)
    return -1;
  return 0;
}

/* ==== Rendezvous service descriptor cache. */
#define REND_CACHE_MAX_AGE 24*60*60
#define REND_CACHE_MAX_SKEW 60*60

typedef struct rend_cache_entry_t {
  int len;
  char *desc;
  rend_service_descriptor_t *parsed;
} rend_cache_entry_t;

static strmap_t *rend_cache = NULL;

void rend_cache_init(void)
{
  rend_cache = strmap_new();
}

void rend_cache_clean(void)
{
  strmap_iter_t *iter;
  const char *key;
  void *val;
  rend_cache_entry_t *ent;
  time_t cutoff;
  cutoff = time(NULL) - REND_CACHE_MAX_AGE;
  for (iter = strmap_iter_init(rend_cache); !strmap_iter_done(iter); ) {
    strmap_iter_get(iter, &key, &val);
    ent = (rend_cache_entry_t*)val;
    if (ent->parsed->timestamp < cutoff) {
      iter = strmap_iter_next_rmv(rend_cache, iter);
      rend_service_descriptor_free(ent->parsed);
      tor_free(ent->desc);
      tor_free(ent);
    } else {
      iter = strmap_iter_next(rend_cache, iter);
    }
  }
}

/* return 1 if query is a valid service id, else return 0. */
int rend_valid_service_id(char *query) {
  if(strlen(query) != REND_SERVICE_ID_LEN)
    return 0;

  if (strspn(query, BASE32_CHARS) != REND_SERVICE_ID_LEN)
    return 0;

  return 1;
}

/* 'query' is a base-32'ed service id. If it's malformed, return -1.
 * Else look it up.
 *   If it is found, point *desc to it, and write its length into
 *   *desc_len, and return 1.
 *   If it is not found, return 0.
 */
int rend_cache_lookup(char *query, const char **desc, int *desc_len)
{
  rend_cache_entry_t *e;
  assert(rend_cache);
  if (!rend_valid_service_id(query))
    return -1;
  e = (rend_cache_entry_t*) strmap_get_lc(rend_cache, query);
  if (!e)
    return 0;
  *desc = e->desc;
  *desc_len = e->len;
  return 1;
}

/* Calculate desc's service id, and store it.
 * Return -1 if it's malformed or otherwise rejected, else return 0.
 */
int rend_cache_store(char *desc, int desc_len)
{
  rend_cache_entry_t *e;
  rend_service_descriptor_t *parsed;
  char query[REND_SERVICE_ID_LEN+1];
  time_t now;
  assert(rend_cache);
  parsed = rend_parse_service_descriptor(desc,desc_len);
  if (!parsed) {
    log_fn(LOG_WARN,"Couldn't parse service descriptor");
    return -1;
  }
  if (rend_get_service_id(parsed->pk, query)<0) {
    log_fn(LOG_WARN,"Couldn't compute service ID");
    rend_service_descriptor_free(parsed);
    return -1;
  }
  now = time(NULL);
  if (parsed->timestamp < now-REND_CACHE_MAX_AGE) {
    log_fn(LOG_WARN,"Service descriptor is too old");
    rend_service_descriptor_free(parsed);
    return -1;
  }
  if (parsed->timestamp > now+REND_CACHE_MAX_SKEW) {
    log_fn(LOG_WARN,"Service descriptor is too far in the future");
    rend_service_descriptor_free(parsed);
    return -1;
  }
  e = (rend_cache_entry_t*) strmap_get_lc(rend_cache, query);
  if (e && e->parsed->timestamp > parsed->timestamp) {
    log_fn(LOG_WARN,"We already have a newer service descriptor with the same ID");
    rend_service_descriptor_free(parsed);
    return -1;
  }
  if (e && e->len == desc_len && !memcmp(desc,e->desc,desc_len)) {
    log_fn(LOG_WARN,"We already have this service descriptor");
    rend_service_descriptor_free(parsed);
    return -1;
  }
  if (!e) {
    e = tor_malloc_zero(sizeof(rend_cache_entry_t));
    strmap_set_lc(rend_cache, query, e);
  } else {
    rend_service_descriptor_free(e->parsed);
    tor_free(e->desc);
  }
  e->parsed = parsed;
  e->len = desc_len;
  e->desc = tor_malloc(desc_len);
  memcpy(e->desc, desc, desc_len);

  log_fn(LOG_INFO,"Successfully stored rend desc '%s', len %d", query, desc_len);
  return 0;
}

/* Dispatch on rendezvous relay command. */
void rend_process_relay_cell(circuit_t *circ, int command, int length,
                             const char *payload)
{
  int r;
  switch(command) {
    case RELAY_COMMAND_ESTABLISH_INTRO:
      r = rend_mid_establish_intro(circ,payload,length);
      break;
    case RELAY_COMMAND_ESTABLISH_RENDEZVOUS:
      r = rend_mid_establish_rendezvous(circ,payload,length);
      break;
    case RELAY_COMMAND_INTRODUCE1:
      r = rend_mid_introduce(circ,payload,length);
      break;
    case RELAY_COMMAND_INTRODUCE2:
      r = rend_service_introduce(circ,payload,length);
      break;
    case RELAY_COMMAND_RENDEZVOUS1:
      r = rend_mid_rendezvous(circ,payload,length);
      break;
    case RELAY_COMMAND_RENDEZVOUS2:
      /* r = rend_client_rendezvous(circ,payload,length); */
      log_fn(LOG_NOTICE, "Ignoring a rendezvous2 cell");
      break;
    default:
      assert(0);
  }
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
