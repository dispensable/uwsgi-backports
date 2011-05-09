#include "uwsgi.h"

extern struct uwsgi_server uwsgi;

struct uwsgi_subscriber_name *uwsgi_get_subscriber(struct uwsgi_dict *udict, char *key, uint16_t keylen) {

	uint64_t ovl;
	struct uwsgi_subscriber *usub;
	struct uwsgi_subscriber_name *ret = NULL;
	
	usub = (struct uwsgi_subscriber *) uwsgi_dict_get(udict, key, keylen, &ovl);

	if (usub == NULL || !ovl) return NULL;

	if (!usub->nodes) return NULL;

	if (usub && ovl) {
		ret = &usub->names[usub->current];
		// dead node
		if (ret->len == 0) {
			if (usub->current == usub->nodes-1) {
				usub->nodes--;
			}
			// retry with another node (if available)
			if (usub->nodes > 0) {
				usub->current++;
				if (usub->current >= usub->nodes) usub->current = 0;
				return uwsgi_get_subscriber(udict, key, keylen);
			}
		}

		if (usub->nodes > 1) {
			usub->current++;
			if (usub->current >= usub->nodes) usub->current = 0;
		}

	}

	return ret;
}

void uwsgi_add_subscriber(struct uwsgi_dict *udict, char *key, uint16_t keylen, char *address, uint64_t address_len) {

	char *ptr;
	uint64_t vallen = 0;
	struct uwsgi_subscriber *usub, nusub;
	int found = 0;
	int i;
	
	ptr = uwsgi_dict_get(udict, key, keylen, &vallen);
	if (ptr && vallen) {
		usub = (struct uwsgi_subscriber *) ptr;
		for(i=0;i<(int)usub->nodes;i++) {
			if (!uwsgi_strncmp(usub->names[i].name, usub->names[i].len, address, address_len)) {
				found = 1;
				break;
			}
		}
		if (!found) {
			found = usub->nodes;
			// check for unallocated slot
			for(i=0;i<(int)usub->nodes;i++) {
				if (usub->names[i].len == 0) {
					found = i;
					break;
				}
			}
			usub->names[found].len = address_len;
			memcpy(usub->names[found].name, address, address_len);
			if (found == (int) usub->nodes) {
				usub->nodes++;
			}
		}
		return;
	}
	else {
		nusub.nodes = 1;
		nusub.current = 0;
		memcpy(nusub.names[0].name, address, address_len);
		uwsgi_dict_set(udict, key, keylen, (char *) &nusub, sizeof(struct uwsgi_subscriber));
	}

}

struct uwsgi_dict *uwsgi_dict_create(uint64_t items, uint64_t blocksize) {

	int i;

	struct uwsgi_dict *udict = (struct uwsgi_dict *) mmap(NULL, sizeof(uint64_t) * UMAX16, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (!udict) {
        	uwsgi_error("mmap()");
                exit(1);
	}

	if (!blocksize) blocksize = 4096;

        if (blocksize % uwsgi.page_size != 0) {
        	uwsgi_log("invalid shared dictionary blocksize %llu: must be a multiple of memory page size (%d bytes)\n", (unsigned long long) udict->blocksize, uwsgi.page_size);
        	exit(1);
	}

	udict->blocksize = blocksize;
	udict->max_items = items;

        udict->hashtable = (uint64_t *) mmap(NULL, sizeof(uint64_t) * UMAX16, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (!udict->hashtable) {
        	uwsgi_error("mmap()");
                exit(1);
	}

        memset(udict->hashtable, 0, sizeof(uint64_t) * UMAX16);

        udict->unused_stack = (uint64_t *) mmap(NULL, sizeof(uint64_t) * udict->max_items, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (!udict->unused_stack) {
        	uwsgi_error("mmap()");
                exit(1);
	}

        memset(udict->unused_stack, 0, sizeof(uint64_t) * udict->max_items);

        udict->items = (struct uwsgi_dict_item *) mmap(NULL, sizeof(struct uwsgi_dict_item) * udict->max_items, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (!udict->items) {
        	uwsgi_error("mmap()");
                exit(1);
        }

        udict->data = mmap(NULL, udict->blocksize * udict->max_items, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (!udict->data) {
        	uwsgi_error("mmap()");
                exit(1);
        }

        for(i=0;i< (int) udict->max_items;i++) {
        	memset(&udict->items[i], 0, sizeof(struct uwsgi_dict_item));
        }

        udict->first_available_item = 1;
        udict->unused_stack_ptr = 0;

        udict->lock = uwsgi_mmap_shared_lock();
        uwsgi_lock_init(udict->lock);

	return udict;
}

uint32_t djb33x_hash(char *key, int keylen) {

	register uint32_t hash = 5381;
	int i;

	for(i=0;i<keylen;i++) {
		hash = ((hash << 5) + hash) ^ key[i];
	}

	return hash;
}


inline uint64_t uwsgi_dict_get_index(struct uwsgi_dict *udict, char *key, uint16_t keylen) {

        uint32_t hash = djb33x_hash(key, keylen);

        int hash_key = hash % 0xffff;

        uint64_t slot = udict->hashtable[hash_key];

        struct uwsgi_dict_item *udi;

        udi = &udict->items[slot];

        // first round
        if (udi->djbhash != hash) goto cycle;
        if (udi->keysize != keylen) goto cycle;
        if (memcmp(udi->key, key, keylen)) goto cycle;

        return slot;

cycle:
        while(udi->next) {
                slot = udi->next;
                udi = &udict->items[slot];
                if (udi->djbhash != hash) continue;
                if (udi->keysize != keylen) continue;
                if (!memcmp(udi->key, key, keylen)) return slot;
        }

        return 0;
}

char *uwsgi_dict_get(struct uwsgi_dict *udict, char *key, uint16_t keylen, uint64_t *valsize) {

        uint64_t index = uwsgi_dict_get_index(udict, key, keylen);

        if (index) {
                *valsize = udict->items[index].valsize;
                udict->items[index].hits++;
                return udict->data+(index*udict->blocksize);
        }

        return NULL;
}

int uwsgi_dict_del(struct uwsgi_dict *udict, char *key, uint16_t keylen) {

        uint64_t index = 0;
        struct uwsgi_dict_item *udi;
        int ret = -1;

        index = uwsgi_dict_get_index(udict, key, keylen);
        if (index) {
                udi = &udict->items[index] ;
                udi->keysize = 0;
                udi->valsize = 0;
                udict->unused_stack_ptr++;
                udict->unused_stack[udict->unused_stack_ptr] = index;
                // try to return to initial condition...
                if (index == udict->first_available_item-1) {
                        udict->first_available_item--;
                }
                ret = 0;
                // relink collisioned entry
                if (udi->prev) {
                        udict->items[udi->prev].next = udi->next;
                }
                if (udi->next) {
                        udict->items[udi->next].prev = udi->prev;
                }
                if (!udi->prev && !udi->next) {
                        // reset hashtable entry
                        udict->hashtable[udi->djbhash % 0xffff] = 0;
                }
                udi->djbhash = 0;
                udi->prev = 0;
                udi->next = 0;
        }

        return ret;
}



inline uint64_t uwsgi_cache_get_index(char *key, uint16_t keylen) {

	uint32_t hash = djb33x_hash(key, keylen);
	
	int hash_key = hash % 0xffff;

	uint64_t slot = uwsgi.cache_hashtable[hash_key];

	struct uwsgi_cache_item *uci;

	//uwsgi_log("found slot %d for key %d\n", slot, hash_key);

	uci = &uwsgi.cache_items[slot];

	// first round
	if (uci->djbhash != hash) goto cycle;
	if (uci->keysize != keylen) goto cycle;
	if (memcmp(uci->key, key, keylen)) goto cycle;

	return slot;

cycle:
	while(uci->next) {
		slot = uci->next;
		uci = &uwsgi.cache_items[slot];
		if (uci->djbhash != hash) continue;
		if (uci->keysize != keylen) continue;
		if (!memcmp(uci->key, key, keylen)) return slot;
	}

	return 0;
}

uint32_t uwsgi_cache_exists(char *key, uint16_t keylen) {

	return uwsgi_cache_get_index(key, keylen);
}

char *uwsgi_cache_get(char *key, uint16_t keylen, uint64_t *valsize) {

	uint64_t index = uwsgi_cache_get_index(key, keylen);

	if (index) {
		if (uwsgi.cache_items[index].flags & UWSGI_CACHE_FLAG_UNGETTABLE) return NULL;
		*valsize = uwsgi.cache_items[index].valsize;
		uwsgi.cache_items[index].hits++;
		return uwsgi.cache+(index*uwsgi.cache_blocksize);
	}

	return NULL;
}

int uwsgi_cache_del(char *key, uint16_t keylen) {

	uint64_t index = 0;
	struct uwsgi_cache_item *uci;
	int ret = -1;

	index = uwsgi_cache_get_index(key, keylen);
	if (index) {
		uci = &uwsgi.cache_items[index] ;
		uci->keysize = 0;
		uci->valsize = 0;
		uwsgi.shared->cache_unused_stack_ptr++;
		uwsgi.cache_unused_stack[uwsgi.shared->cache_unused_stack_ptr] = index;
		// try to return to initial condition...
		if (index == uwsgi.shared->cache_first_available_item-1) {
			uwsgi.shared->cache_first_available_item--;
			//uwsgi_log("FACI: %llu STACK PTR: %llu\n", (unsigned long long) uwsgi.shared->cache_first_available_item, (unsigned long long) uwsgi.shared->cache_unused_stack_ptr);
		}
		ret = 0;
		// relink collisioned entry
		if (uci->prev) {
			uwsgi.cache_items[uci->prev].next = uci->next;	
		}
		if (uci->next) {
			uwsgi.cache_items[uci->next].prev = uci->prev;	
		}
		if (!uci->prev && !uci->next) {
			// reset hashtable entry
			//uwsgi_log("!!! resetted hashtable entry !!!\n");
			uwsgi.cache_hashtable[uci->djbhash % 0xffff] = 0;
		}
		uci->djbhash = 0;
		uci->prev = 0;
		uci->next = 0;
	}

	return ret;
}

void uwsgi_cache_fix() {

	uint64_t i;

	for(i=0;i< uwsgi.cache_max_items;i++) {
		// valid record ?
		if (uwsgi.cache_items[i].keysize) {
			if (!uwsgi.cache_items[i].prev) {
				// put value in hash_table
				uwsgi.cache_hashtable[ uwsgi.cache_items[i].djbhash % 0xffff] = i;
			}
		}
		else {
			// put this record in unused stack
			uwsgi.shared->cache_first_available_item = i;
			uwsgi.shared->cache_unused_stack_ptr++;
			uwsgi.cache_unused_stack[uwsgi.shared->cache_unused_stack_ptr] = i;
		}
	}
}

int uwsgi_cache_set(char *key, uint16_t keylen, char *val, uint64_t vallen, uint64_t expires, uint16_t flags) {

	uint64_t index = 0, last_index = 0 ;

	struct uwsgi_cache_item *uci, *ucii;

	int ret = -1;
	int slot;

	if (!keylen || !vallen) return -1;

	if (keylen > UWSGI_CACHE_MAX_KEY_SIZE) return -1;	

	if (uwsgi.shared->cache_first_available_item >= uwsgi.cache_max_items && !uwsgi.shared->cache_unused_stack_ptr) {
		uwsgi_log("*** DANGER cache is FULL !!! ***\n");
		goto end;
	}

	//uwsgi_log("putting cache data in key %.*s %d\n", keylen, key, vallen);
	index = uwsgi_cache_get_index(key, keylen);
	if (!index) {
		if (uwsgi.shared->cache_unused_stack_ptr) {
			//uwsgi_log("!!! REUSING CACHE SLOT !!! (faci: %llu)\n", (unsigned long long) uwsgi.shared->cache_first_available_item);
			index = uwsgi.cache_unused_stack[uwsgi.shared->cache_unused_stack_ptr];
			uwsgi.shared->cache_unused_stack_ptr--;
		}
		else {
			index = uwsgi.shared->cache_first_available_item;	
			if (uwsgi.shared->cache_first_available_item < uwsgi.cache_max_items) {
				uwsgi.shared->cache_first_available_item++;
			}
		}
		uci = &uwsgi.cache_items[index] ;
		if (expires) expires += time(NULL);
		uci->expires = expires;
		uci->djbhash = djb33x_hash(key, keylen);
		uci->hits = 0;
		uci->flags = flags;
		memcpy(uci->key, key, keylen);
		memcpy(uwsgi.cache+(index*uwsgi.cache_blocksize), val, vallen);

		// set this as late as possibile (to reduce races risk)

		uci->valsize = vallen;
		uci->keysize = keylen;	
		ret = 0;
		// now put the value in the 16bit hashtable
		slot = uci->djbhash % 0xffff;

		if (uwsgi.cache_hashtable[slot] == 0) {
			uwsgi.cache_hashtable[slot] = index;
		}
		else {
			//uwsgi_log("HASH COLLISION !!!!\n");
			// append to first available next
			last_index = uwsgi.cache_hashtable[slot];
			ucii = &uwsgi.cache_items[ last_index ];
			while(ucii->next) {
				last_index = ucii->next;
				ucii = &uwsgi.cache_items[ last_index ];
			}
			ucii->next = index;
			uci->prev = last_index;
		}
	}

end:
	return ret;
	
}

int uwsgi_dict_set(struct uwsgi_dict *udict, char *key, uint16_t keylen, char *val, uint64_t vallen) {

        uint64_t index = 0, last_index = 0 ;

        struct uwsgi_dict_item *udi, *udii;

        int ret = -1;
        int slot;

        if (!keylen || !vallen) return -1;

        if (keylen > UWSGI_CACHE_MAX_KEY_SIZE) return -1;

        if (udict->first_available_item >= udict->max_items && !udict->unused_stack_ptr) {
                uwsgi_log("*** DANGER dictionary %p is FULL !!! ***\n", udict);
                goto end;
        }

        index = uwsgi_dict_get_index(udict, key, keylen);
        if (!index) {
                if (udict->unused_stack_ptr) {
                        index = udict->unused_stack[udict->unused_stack_ptr];
                        udict->unused_stack_ptr--;
                }
                else {
                        index = udict->first_available_item;
                        if (udict->first_available_item < udict->max_items) {
                                udict->first_available_item++;
                        }
                }
                udi = &udict->items[index] ;
                udi->djbhash = djb33x_hash(key, keylen);
                udi->hits = 0;
                memcpy(udi->key, key, keylen);
                memcpy(udict->data+(index*udict->blocksize), val, vallen);

                // set this as late as possibile (to reduce races risk)

                udi->valsize = vallen;
                udi->keysize = keylen;
                ret = 0;
                // now put the value in the 16bit hashtable
                slot = udi->djbhash % 0xffff;

                if (udict->hashtable[slot] == 0) {
                        udict->hashtable[slot] = index;
                }
                else {
                        // append to first available next
                        last_index = udict->hashtable[slot];
                        udii = &udict->items[ last_index ];
                        while(udii->next) {
                                last_index = udii->next;
                                udii = &udict->items[ last_index ];
                        }
                        udii->next = index;
                        udi->prev = last_index;
                }
        }

end:
        return ret;

}


void cache_command(char *key, uint16_t keylen, char *val, uint16_t vallen, void *data) {

	struct wsgi_request *wsgi_req = (struct wsgi_request *) data;
	uint64_t tmp_vallen = 0;

	if (vallen > 0) {
		if (!uwsgi_strncmp(key, keylen, "key", 3)) {
			val = uwsgi_cache_get(val, vallen, &tmp_vallen);
                        if (val && tmp_vallen > 0) {
                        	wsgi_req->response_size = write(wsgi_req->poll.fd, val, tmp_vallen);
                        }

		}		
		else if (!uwsgi_strncmp(key, keylen, "get", 3)) {
			val = uwsgi_cache_get(val, vallen, &tmp_vallen);
			if (val && vallen > 0) {
                        	wsgi_req->response_size = write(wsgi_req->poll.fd, val, tmp_vallen);
			}
			else {
				wsgi_req->response_size = write(wsgi_req->poll.fd, "HTTP/1.0 404 Not Found\r\n\r\n<h1>Not Found</h1>", 44);
			}
		}
	}
}

int uwsgi_cache_request(struct wsgi_request *wsgi_req) {

	uint64_t vallen = 0;
	char *value;
	char *argv[3];
	uint8_t argc = 0;

	switch(wsgi_req->uh.modifier2) {
		case 0:
			// get
			if (wsgi_req->uh.pktsize > 0) {
				value = uwsgi_cache_get(wsgi_req->buffer, wsgi_req->uh.pktsize, &vallen);
				if (value && vallen > 0) {
					wsgi_req->uh.pktsize = vallen;
					wsgi_req->response_size = write(wsgi_req->poll.fd, &wsgi_req->uh, 4);
					wsgi_req->response_size += write(wsgi_req->poll.fd, value, vallen);
				}
			}
			break;		
		case 1:
			// set
			if (wsgi_req->uh.pktsize > 0) {
				argc = 3;
				if (!uwsgi_parse_array(wsgi_req->buffer, wsgi_req->uh.pktsize, argv, &argc)) {
					if (argc > 1) {
						uwsgi_cache_set(argv[0], strlen(argv[0]), argv[1], strlen(argv[1]), 0, 0);
					}
				}
			}
			break;
		case 2:
			// del
			if (wsgi_req->uh.pktsize > 0) {
				uwsgi_cache_del(wsgi_req->buffer, wsgi_req->uh.pktsize);
			}
			break;
		case 3:
		case 4:
			// dict
			if (wsgi_req->uh.pktsize > 0) {
				uwsgi_hooked_parse(wsgi_req->buffer, wsgi_req->uh.pktsize, cache_command, (void *) wsgi_req);
			}
			break;
		case 5:
			// get (uwsgi + stream)
			if (wsgi_req->uh.pktsize > 0) {
				value = uwsgi_cache_get(wsgi_req->buffer, wsgi_req->uh.pktsize, &vallen);
				if (value && vallen > 0) {
					wsgi_req->uh.pktsize = 0;
					wsgi_req->uh.modifier2 = 1;
					wsgi_req->response_size = write(wsgi_req->poll.fd, &wsgi_req->uh, 4);
					wsgi_req->response_size += write(wsgi_req->poll.fd, value, vallen);
				}
				else {
					wsgi_req->uh.pktsize = 0;
					wsgi_req->uh.modifier2 = 0;
					wsgi_req->response_size = write(wsgi_req->poll.fd, &wsgi_req->uh, 4);
				}
			}
			break;		
	}

	return 0;
}

struct uwsgi_plugin uwsgi_cache_plugin = {

        .name = "cache",
        .modifier1 = 111, 
        .request = uwsgi_cache_request,

};
