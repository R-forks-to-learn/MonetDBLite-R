#define RSTR(somestr) mkCharCE(somestr, CE_UTF8)

#define INT64_PTR(X) ((int64_t*) NUMERIC_POINTER(X))

#define BAT_TO_SXP(bat,n,tpe,retsxp,newfun,ptrfun,ctype,naval,memcopy,nacheck)\
	do {													\
		tpe v; BUN j;     								\
		ctype *valptr = NULL;                               \
		tpe* p = (tpe*) Tloc(bat, 0);                       \
		retsxp = PROTECT(newfun(n));		    \
		if (!retsxp) break;                                 \
		valptr = ptrfun(retsxp);                            \
		if (bat->tnonil && !bat->tnil) {                    \
			if (memcopy) {									\
				memcpy(valptr, p,                           \
					n * sizeof(tpe));           \
			} else {                                        \
				for (j = 0; j < n; j++) {\
					valptr[j] = (ctype) p[j];               \
				}                                           \
			} 												\
		} else {                                            \
		for (j = 0; j < n; j++) {		                    \
			v = p[j];                                       \
			if (nacheck)							\
				valptr[j] = naval;	                        \
			else											\
				valptr[j] = (ctype) v;	                    \
		}}													\
	} while (0)

#define BAT_TO_INTSXP(bat,n,tpe,retsxp,memcopy)						\
	BAT_TO_SXP(bat,n,tpe,retsxp,NEW_INTEGER,INTEGER_POINTER,int,NA_INTEGER,memcopy,v == tpe##_nil)\

#define BAT_TO_REALSXP(bat,n,tpe,retsxp,memcopy)						\
	BAT_TO_SXP(bat,n,tpe,retsxp,NEW_NUMERIC,NUMERIC_POINTER,double,NA_REAL,memcopy,isnan(v))\

#define SXP_TO_BAT(tpe,access_fun,na_check)								\
	do {																\
		tpe *p, prev = tpe##_nil; BUN j;								\
		b = COLnew(0, TYPE_##tpe, cnt, TRANSIENT);						\
		if (!b) break;                                                  \
		b->tnil = 0; b->tnonil = 1; b->tkey = 0;						\
		b->tsorted = 1; b->trevsorted = 1;b->tdense = 0;				\
		p = (tpe*) Tloc(b, 0);								\
		for(j = 0; j < cnt; j++, p++){								    \
			*p = (tpe) access_fun(s)[j];							    \
			if (na_check){ b->tnil = 1; 	b->tnonil = 0; 	*p= tpe##_nil;} \
			if (j > 0){													\
				if (*p > prev && b->trevsorted){						\
					b->trevsorted = 0;									\
				} else													\
					if (*p < prev && b->tsorted){						\
						b->tsorted = 0;									\
					}													\
			}															\
			prev = *p;													\
		}																\
		BATsetcount(b, cnt);											\
		BATsettrivprop(b);												\
	} while (0)

#ifndef NATIVE_WIN32

typedef struct R_MASQ_BAT {
	char* base_map;
	char* data_map;
	char* sexp_ptr;
	size_t data_map_len;
	size_t data_len;
	bat bat_cache_id;
} R_MASQ_BAT;

static void monetdb_r_masq_free(R_MASQ_BAT* masq) {
	if (masq == NULL) {
		return;
	}
	if (masq->data_map != NULL) {
		munmap(masq->data_map, masq->data_map_len);
	}
	if (masq->base_map != NULL) {
		munmap(masq->base_map, masq->data_map_len + MT_pagesize());
	}
	free(masq);
}

static void* monetdb_r_alloc(R_allocator_t *allocator, size_t length) {
	R_MASQ_BAT* masq = (R_MASQ_BAT*) allocator->data;
	// R normalizes vector lengths to a multiple of 8Byte
	size_t data_len = (((masq->data_len-1) / sizeof(double)) + 1) * sizeof(double);

	if (length <= masq->data_len || (length - data_len) > MT_pagesize()) {
		error("Wrong header size");
		return NULL;
	}
	masq->sexp_ptr = masq->data_map - (length - data_len);
	return masq->sexp_ptr;
}

static void monetdb_r_free(R_allocator_t *allocator, void *ptr) {
	// TODO: ptr might be offset for long vectors?! check this
	R_MASQ_BAT* masq = (R_MASQ_BAT*) allocator->data;
	if (ptr != masq->sexp_ptr) {
		error("Pointer address mismatch");
		return;
	}
	BBPunfix(masq->bat_cache_id);
	monetdb_r_masq_free(masq);
}

#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS		MAP_ANON
#endif

static SEXP monetdb_r_dressup(BAT *b, size_t n, SEXPTYPE target_type) {
	R_MASQ_BAT* masq = malloc(sizeof(R_MASQ_BAT));
	SEXP varvalue;
	R_allocator_t allocator;

	char* filename = GDKfilepath(b->T.heap.farmid, BATDIR, b->T.heap.filename, NULL);
	int fd = -1;
	if (!masq || !filename) {
		return NULL;
	}

	// secret mmap sauce follows
	masq->data_map_len = b->T.heap.size;
	fd = open(filename, O_RDONLY, NULL);
	GDKfree(filename);
	masq->base_map = mmap(NULL,                           masq->data_map_len + MT_pagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	masq->data_map = mmap(masq->base_map + MT_pagesize(), masq->data_map_len,                 PROT_READ,              MAP_PRIVATE | MAP_FIXED,     fd, 0);
	masq->bat_cache_id = b->batCacheid;
	masq->data_len = n * ATOMsize(b->ttype);
	close(fd);

	// check if the MAP_FIXED worked as expected
	if (masq->base_map == NULL ||  masq->data_map == NULL || masq->base_map + MT_pagesize() != masq->data_map) {
		monetdb_r_masq_free(masq);
		return NULL;
	}

	allocator.mem_alloc = monetdb_r_alloc;
	allocator.mem_free  = monetdb_r_free;
	allocator.res = NULL;
	allocator.data = masq;

	// pointer fun, we know we are allowed to write there
	// call R's own allocator to set up various structures for us
	varvalue = PROTECT(allocVector3(target_type, n, &allocator));
	SET_NAMED(varvalue, 1);
	return varvalue;
}

#endif


static SEXP bat_to_sexp(BAT* b, size_t n, sql_subtype *subtype, int *unfix, char int64) {
	SEXP varvalue = NULL;
	int battype = getBatType(b->ttype);
	// TODO: deal with more esoteric SQL types (TIME)

	// special snowflake: decimal to double, needs two step conversion
	if (subtype && subtype->type && subtype->type->sqlname &&
			strcmp("decimal", subtype->type->sqlname) == 0) {
		// borrowed from pyapi
		int bat_type = ATOMstorage(battype);
		int hpos = subtype->scale;
		bat result = 0;
		str res;
		// decimal values can be stored in various numeric fields, so check the
		// numeric field and convert the one it's actually stored in
		switch (bat_type) {
			case TYPE_bte:
				res = batbte_dec2_dbl(&result, &hpos, &b->batCacheid);
				break;
			case TYPE_sht:
				res = batsht_dec2_dbl(&result, &hpos, &b->batCacheid);
				break;
			case TYPE_int:
				res = batint_dec2_dbl(&result, &hpos, &b->batCacheid);
				break;
			case TYPE_lng:
				res = batlng_dec2_dbl(&result, &hpos, &b->batCacheid);
				break;
			default:
				return NULL;
		}
		if (res != MAL_SUCCEED) {
			return NULL;
		}
		b = BATdescriptor(result);
		battype = TYPE_dbl;
		// now convert double to sexp later normally, using zero-copy if applicable
	}
	if (battype == TYPE_bte) {
		BAT_TO_INTSXP(b, n, bte, varvalue, 0);
	} else if (battype == TYPE_void) {
		size_t i = 0;
		varvalue = PROTECT(NEW_LOGICAL(n));
		if (!varvalue) {
			return NULL;
		}
		for (i = 0; i < n; i++) {
			LOGICAL_POINTER(varvalue)[i] = NA_LOGICAL;
		}
	} else if (battype == TYPE_bit) {
		BAT_TO_SXP(b, n, bte, varvalue, NEW_LOGICAL, LOGICAL_POINTER, int, NA_LOGICAL, 0, v == bit_nil);
	} else if (battype == TYPE_sht) {
		BAT_TO_INTSXP(b, n, sht, varvalue, 0);
	} else if (battype == TYPE_int) {
#ifndef NATIVE_WIN32
		// special case: bulk memcpy/masquerade
		if (b->T.heap.storage != STORE_MMAP ||
				n < 2000000) {
			BAT_TO_INTSXP(b, n, int, varvalue, 1);
		} else {
			varvalue = monetdb_r_dressup(b, n, INTSXP);
			*unfix = 0;
		}
#else
		BAT_TO_INTSXP(b, n, int, varvalue, 1);
#endif
	}
#ifdef HAVE_HGE
	else if   (battype == TYPE_hge) { /* R's integers are stored as int, so we cannot be sure hge will fit */
			BAT_TO_REALSXP(b, n, hge, varvalue, 0);
	}
#endif
	else if   (battype == TYPE_flt) {
		BAT_TO_REALSXP(b, n, flt, varvalue, 0);
	} else if (battype == TYPE_dbl) {
#ifndef NATIVE_WIN32
		// special case: bulk memcpy/masquerade, but only if there are no NULLs
		if (!b->tnonil || b->tnil || b->T.heap.storage != STORE_MMAP ||
				n < 1000000) {
			BAT_TO_REALSXP(b, n, dbl, varvalue, 1);
		} else {
			varvalue = monetdb_r_dressup(b, n, REALSXP);
			*unfix = 0;
		}
#else
			BAT_TO_REALSXP(b, n, dbl, varvalue, 1);
#endif
	} else if (battype == TYPE_lng) {
		if (!int64){
			BAT_TO_SXP(b,n,lng,varvalue,NEW_NUMERIC,NUMERIC_POINTER,double,NA_REAL,0,v == lng_nil);
		}
		else {
#ifndef NATIVE_WIN32
			if (!b->tnonil || b->tnil || b->T.heap.storage != STORE_MMAP ||
						n < 1000000) {
				BAT_TO_REALSXP(b, n, dbl, varvalue, 1);
			} else {
				varvalue = monetdb_r_dressup(b, n, REALSXP);
				*unfix = 0;
			}
#else
			BAT_TO_REALSXP(b, n, dbl, varvalue, 1);
#endif
			SET_CLASS(varvalue, PROTECT(mkString("integer64")));
			UNPROTECT(1);
		}


	} else if (battype == TYPE_str) {
		BUN j = 0;
		BATiter li = bat_iterator(b);
		varvalue = PROTECT(NEW_STRING(n));
		if (varvalue == NULL) {
			return NULL;
		}
		/* special case where we exploit the duplicate-eliminated string heap */
		if (GDK_ELIMDOUBLES(b->tvheap)) {
			size_t n_protects = 0;
			SEXP* sexp_ptrs = GDKzalloc(b->tvheap->free * sizeof(SEXP));
			if (!sexp_ptrs) {
				return NULL;
			}
			for (j = 0; j < n; j++) {
				const char *t = (const char *) BUNtvar(li, j);
				ptrdiff_t offset = t - b->tvheap->base;
				if (!sexp_ptrs[offset]) {
					if (strcmp(t, str_nil) == 0) {
						sexp_ptrs[offset] = NA_STRING;
					} else {
						sexp_ptrs[offset] = PROTECT(RSTR(t));
						n_protects++;
					}
				}
				assert(sexp_ptrs[offset]);
				SET_STRING_ELT(varvalue, j, sexp_ptrs[offset]);
			}
			UNPROTECT(n_protects);
			GDKfree(sexp_ptrs);
		}
		else {

			if (b->tnonil) {
				for (j = 0; j < n; j++) {
					SET_STRING_ELT(varvalue, j, PROTECT(RSTR(
						(const char *) BUNtvar(li, j))));
					UNPROTECT(1);
				}
			}
			else {
				for (j = 0; j < n; j++) {
					const char *t = (const char *) BUNtvar(li, j);
					if (strcmp(t, str_nil) == 0) {
						SET_STRING_ELT(varvalue, j, NA_STRING);
					} else {
						SET_STRING_ELT(varvalue, j, PROTECT(RSTR(t)));
						UNPROTECT(1);
					}
				}
			}
		}
	} else if (battype == TYPE_date && ATOMstorage(battype) == TYPE_int) {
		ValRecord val;
		BAT *b2;
		val.vtype = TYPE_dbl;
		val.val.dval = 719528;
		b2 = BATcalcsubcst(b, &val, NULL, TYPE_dbl, 1);
		if(!b2) {
			return NULL;
		}
		varvalue = bat_to_sexp(b2, n, NULL, unfix, int64);
		if (!varvalue) {
			return NULL;
		}
		SET_CLASS(varvalue, PROTECT(mkString("Date")));
	    UNPROTECT(1);
	} else if (battype == TYPE_timestamp && ATOMstorage(battype) == TYPE_lng) {
		BUN j;
		const timestamp *t = (const timestamp *) Tloc(b, 0);
		double *valptr = NULL;
	    SEXP class = NULL;
		timestamp epoch;

		varvalue = PROTECT(NEW_NUMERIC(n));
	    class = PROTECT(NEW_STRING(2));

		if (!varvalue || !class || MTIMEunix_epoch(&epoch) != MAL_SUCCEED) {
			return NULL;
		}

	    SET_STRING_ELT(class, 0, PROTECT(mkChar("POSIXct")));
	    SET_STRING_ELT(class, 1, PROTECT(mkChar("POSIXt")));
		SET_CLASS(varvalue, class);
		setAttrib(varvalue, install("tzone"), PROTECT(mkString("UTC")));
		UNPROTECT(4);

		valptr = NUMERIC_POINTER(varvalue);
		for (j = 0; j < n; j++, t++) {
			if (ts_isnil(*t)) {
				valptr[j] = NA_REAL;
			} else {
				valptr[j] = ((double) (t->days - epoch.days)) * ((lng) 24 * 60 * 60) + ((double) (t->msecs - epoch.msecs)/1000);
			}
		}
	} else if (battype == TYPE_sqlblob) {
		BUN j;
		BATiter li = bat_iterator(b);
		varvalue = PROTECT(NEW_LIST(n));
		if (!varvalue) {
			return NULL;
		}

		for (j = 0; j < n; j++) {
			blob *t = (blob*) BUNtvar(li, j);
			if (t->nitems == ~(size_t) 0) {
				SET_VECTOR_ELT(varvalue, j, ScalarLogical(NA_LOGICAL));
			}
			else {
				SEXP rawval = NEW_RAW(t->nitems);
				if (!rawval) {
					return NULL;
				}
				memcpy(RAW_POINTER(rawval), t->data, t->nitems);
				SET_VECTOR_ELT(varvalue, j, rawval);
			}
		}
	} else if (battype == TYPE_daytime && ATOMstorage(battype) == TYPE_int) {
		BUN j;
		const daytime *t = (const daytime *) Tloc(b, 0);
		double *valptr = NULL;

		varvalue = PROTECT(NEW_NUMERIC(n));
		if (!varvalue) {
			return NULL;
		}
		SET_CLASS(varvalue, PROTECT(mkString("difftime")));
		setAttrib(varvalue, install("units"), PROTECT(mkString("hours")));
		UNPROTECT(2);

		valptr = NUMERIC_POINTER(varvalue);
		for (j = 0; j < n; j++, t++) {
			if (*t == daytime_nil) {
				valptr[j] = NA_REAL;
			} else {
				daytime n = *t;
				int h;
				double frac;
				h = n / 3600000;
				n -= h * 3600000;
				frac = (n / 60000.0)/60.0;
				valptr[j]  = h + frac;
			}
		}
	}

	return varvalue;
}

static int is_single_NA(SEXP v) {
	if (!v) return 0;
	if (LENGTH(v) != 1) return 0;
    switch (TYPEOF(v)) {
    case REALSXP:
    	return ISNAN(REAL(v)[0]);
    case INTSXP:
    	return INTEGER(v)[0] == NA_INTEGER;
    case LGLSXP:
    	return LOGICAL(v)[0] == NA_LOGICAL;
    }
    return 0;
}

static BAT* sexp_to_bat(SEXP s, int type) {
	BAT* b = NULL;
	BUN cnt = LENGTH(s);
	switch (type) {
	case TYPE_int: {
		if (!IS_INTEGER(s)) {
			return NULL;
		}
		SXP_TO_BAT(int, INTEGER_POINTER, *p==NA_INTEGER);
		break;
	}
	case TYPE_lng: {
		if (strcmp("integer64", CHAR(STRING_ELT(GET_CLASS(s), 0))) == 0) {
			if (!IS_NUMERIC(s)) {
				return NULL;
			}
			SXP_TO_BAT(lng, INT64_PTR, *p==NA_INTEGER);
		} else {
			if (!IS_INTEGER(s)) {
				return NULL;
			}
			SXP_TO_BAT(lng, INTEGER_POINTER, *p==NA_INTEGER);
		}
		break;
	}
#ifdef HAVE_HGE
	case TYPE_hge: {
		if (!IS_INTEGER(s)) {
			return NULL;
		}
		SXP_TO_BAT(hge, INTEGER_POINTER, *p==NA_INTEGER);
		break;
	}
#endif
	case TYPE_bte:
	case TYPE_bit: { // only R logical types fit into bit BATs
		if (!IS_LOGICAL(s)) {
			return NULL;
		}
		// logicals are 4-byte integers in R, we lose NA's if we compare after the cast to char
		SXP_TO_BAT(bit, LOGICAL_POINTER, LOGICAL_POINTER(s)[j] == NA_LOGICAL);
		break;
	}
	case TYPE_dbl: {
		if (!IS_NUMERIC(s)) {
			return NULL;
		}
		SXP_TO_BAT(dbl, NUMERIC_POINTER, (ISNA(*p) || isnan(*p) || isinf(*p)));
		break;
	}
	case TYPE_str: {
		SEXP levels;
		size_t j;
		if (!IS_CHARACTER(s) && !isFactor(s)) {
			return NULL;
		}
		b = COLnew(0, TYPE_str, cnt, TRANSIENT);
		if (!b) return NULL;
		BATsettrivprop(b);
		/* get levels once, since this is a function call */
		levels = GET_LEVELS(s);

		for (j = 0; j < cnt; j++) {
			SEXP rse;
			if (isFactor(s)) {
				int ii = INTEGER(s)[j];
				if (ii == NA_INTEGER) {
					rse = NA_STRING;
				} else {
					rse = STRING_ELT(levels, ii - 1);
				}
			} else {
				rse = STRING_ELT(s, j);
			}
			if (rse == NA_STRING) {
				b->tnil = 1;
				b->tnonil = 0;
				if (BUNappend(b, str_nil, FALSE) != GDK_SUCCEED) {
					BBPreclaim(b);
					b = NULL;
				}
			} else {
				if (BUNappend(b, CHAR(rse), FALSE) != GDK_SUCCEED) {
					BBPreclaim(b);
					b = NULL;
				}
			}
		}
		break;
	}
	}

	// types below are dynamic so we can't switch
	if (type == TYPE_sqlblob && IS_LIST(s)) {
		size_t i = 0;
		var_t bun_offset = 0;
		blob *ele_blob;
		b = COLnew(0, TYPE_sqlblob, cnt, TRANSIENT);
		BATsettrivprop(b);
		if (!b) return NULL;
		for (i = 0; i < cnt; i++) {
			SEXP list_ele = VECTOR_ELT(s, i);
			size_t blob_len = LENGTH(list_ele);
			char free_blob = FALSE;
			if (!list_ele || !(IS_RAW(list_ele) || is_single_NA(list_ele))) return NULL; // FIXME
			if (IS_RAW(list_ele)) {
				ele_blob = GDKmalloc(blobsize(blob_len));
				free_blob = TRUE;
				if (!ele_blob) {
					return NULL;
				}
				ele_blob->nitems = blob_len;
				memcpy(ele_blob->data, RAW_POINTER(list_ele), blob_len);
			} else {
				b->tnil = 1;
				b->tnonil = 0;
				ele_blob = (blob*) BLOBnull();
			}
			BLOBput(b->tvheap, &bun_offset, ele_blob);
			if (BUNappend(b, ele_blob, FALSE) != GDK_SUCCEED) {
				if (free_blob) {
					GDKfree(ele_blob);
				}
				return NULL;
			}
			if (free_blob) {
				GDKfree(ele_blob);
			}
		}
	}

	if (type == TYPE_date && (IS_NUMERIC(s) || IS_INTEGER(s)) && strcmp("Date", CHAR(STRING_ELT(GET_CLASS(s), 0))) == 0) {
		ValRecord val;
		val.vtype = TYPE_int;
		val.val.ival = 719528;
		if (IS_INTEGER(s)) {
			SXP_TO_BAT(int, INTEGER_POINTER, *p==NA_INTEGER);
		} else {
			SXP_TO_BAT(int, NUMERIC_POINTER, ISNA(NUMERIC_POINTER(s)[j]));
		}
		b = BATcalcaddcst(b, &val, NULL, TYPE_int, 1);
		b->ttype = TYPE_date;
	}

	if (type == TYPE_timestamp && IS_NUMERIC(s) && strcmp("POSIXct", CHAR(STRING_ELT(GET_CLASS(s), 0))) == 0) {
		size_t j; timestamp *p, epoch;
		b = COLnew(0, TYPE_timestamp, cnt, TRANSIENT);
		if (!b || MTIMEunix_epoch(&epoch) != MAL_SUCCEED) return NULL;
		BATsettrivprop(b);

		for (j = 0, p = (timestamp*) Tloc(b, 0); j < cnt; j++, p++) {
			double dp = NUMERIC_POINTER(s)[j];
			if (ISNA(dp)) {
				b->tnil = 1;
				b->tnonil = 0;
				*p = *timestamp_nil;
			} else {
				lng in = (lng) dp * 1000;
				MTIMEtimestamp_add(p, &epoch, &in);
			}
		}
	}

	if (b) {
		BATsetcount(b, cnt);
		BBPkeepref(b->batCacheid);
	}
	return b;
}
