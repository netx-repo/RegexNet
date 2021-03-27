#include <stdio.h>

#include <common/cfgparse.h>
#include <common/chunk.h>
#include <common/buffer.h>
#include <common/errors.h>
#include <proto/arg.h>
#include <proto/log.h>
#include <proto/proto_http.h>
#include <proto/sample.h>
#include <import/xxhash.h>
#include <import/lru.h>
#include <51Degrees.h>

struct _51d_property_names {
	struct list list;
	char *name;
};

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
static struct lru64_head *_51d_lru_tree = NULL;
static unsigned long long _51d_lru_seed;
#endif

static struct {
	char property_separator;    /* the separator to use in the response for the values. this is taken from 51degrees-property-separator from config. */
	struct list property_names; /* list of properties to load into the data set. this is taken from 51degrees-property-name-list from config. */
	char *data_file_path;
	int header_count; /* number of HTTP headers related to device detection. */
	struct chunk *header_names; /* array of HTTP header names. */
	fiftyoneDegreesDataSet data_set; /* data set used with the pattern and trie detection methods. */
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	fiftyoneDegreesWorksetPool *pool; /* pool of worksets to avoid creating a new one for each request. */
#endif
#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
	int32_t *header_offsets; /* offsets to the HTTP header name string. */
	fiftyoneDegreesDeviceOffsets device_offsets; /* Memory used for device offsets. */
#endif
	int cache_size;
} global_51degrees = {
	.property_separator = ',',
	.property_names = LIST_HEAD_INIT(global_51degrees.property_names),
	.data_file_path = NULL,
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	.data_set = { },
#endif
	.cache_size = 0,
};

static int _51d_data_file(char **args, int section_type, struct proxy *curpx,
                          struct proxy *defpx, const char *file, int line,
                          char **err)
{
	if (*(args[1]) == 0) {
		memprintf(err,
		          "'%s' expects a filepath to a 51Degrees trie or pattern data file.",
		          args[0]);
		return -1;
	}

	if (global_51degrees.data_file_path)
		free(global_51degrees.data_file_path);
	global_51degrees.data_file_path = strdup(args[1]);

	return 0;
}

static int _51d_property_name_list(char **args, int section_type, struct proxy *curpx,
                                   struct proxy *defpx, const char *file, int line,
                                   char **err)
{
	int cur_arg = 1;
	struct _51d_property_names *name;

	if (*(args[cur_arg]) == 0) {
		memprintf(err,
		          "'%s' expects at least one 51Degrees property name.",
		          args[0]);
		return -1;
	}

	while (*(args[cur_arg])) {
		name = calloc(1, sizeof(*name));
		name->name = strdup(args[cur_arg]);
		LIST_ADDQ(&global_51degrees.property_names, &name->list);
		++cur_arg;
	}

	return 0;
}

static int _51d_property_separator(char **args, int section_type, struct proxy *curpx,
                                   struct proxy *defpx, const char *file, int line,
                                   char **err)
{
	if (*(args[1]) == 0) {
		memprintf(err,
		          "'%s' expects a single character.",
		          args[0]);
		return -1;
	}
	if (strlen(args[1]) > 1) {
		memprintf(err,
		          "'%s' expects a single character, got '%s'.",
		          args[0], args[1]);
		return -1;
	}

	global_51degrees.property_separator = *args[1];

	return 0;
}

static int _51d_cache_size(char **args, int section_type, struct proxy *curpx,
                           struct proxy *defpx, const char *file, int line,
                           char **err)
{
	if (*(args[1]) == 0) {
		memprintf(err,
		          "'%s' expects a positive numeric value.",
		          args[0]);
		return -1;
	}

	global_51degrees.cache_size = atoi(args[1]);
	if (global_51degrees.cache_size < 0) {
		memprintf(err,
		          "'%s' expects a positive numeric value, got '%s'.",
		          args[0], args[1]);
		return -1;
	}

	return 0;
}

static int _51d_fetch_check(struct arg *arg, char **err_msg)
{
	if (global_51degrees.data_file_path)
		return 1;

	memprintf(err_msg, "51Degrees data file is not specified (parameter '51degrees-data-file')");
	return 0;
}

static int _51d_conv_check(struct arg *arg, struct sample_conv *conv,
                           const char *file, int line, char **err_msg)
{
	if (global_51degrees.data_file_path)
		return 1;

	memprintf(err_msg, "51Degrees data file is not specified (parameter '51degrees-data-file')");
	return 0;
}

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
static void _51d_lru_free(void *cache_entry)
{
	struct chunk *ptr = cache_entry;

	if (!ptr)
		return;

	free(ptr->str);
	free(ptr);
}

/* Allocates memory freeing space in the cache if necessary.
*/
static void *_51d_malloc(int size)
{
	void *ptr = malloc(size);

	if (!ptr) {
		/* free the oldest 10 entries from lru to free up some memory
		 * then try allocating memory again */
		lru64_kill_oldest(_51d_lru_tree, 10);
		ptr = malloc(size);
	}

	return ptr;
}

/* Insert the data associated with the sample into the cache as a fresh item.
 */
static void _51d_insert_cache_entry(struct sample *smp, struct lru64 *lru, void* domain)
{
	struct chunk *cache_entry = _51d_malloc(sizeof(*cache_entry));

	if (!cache_entry)
		return;

	cache_entry->str = _51d_malloc(smp->data.u.str.len + 1);
	if (!cache_entry->str) {
		free(cache_entry);
		return;
	}

	memcpy(cache_entry->str, smp->data.u.str.str, smp->data.u.str.len);
	cache_entry->str[smp->data.u.str.len] = 0;
	cache_entry->len = smp->data.u.str.len;
	lru64_commit(lru, cache_entry, domain, 0, _51d_lru_free);
}

/* Retrieves the data from the cache and sets the sample data to this string.
 */
static void _51d_retrieve_cache_entry(struct sample *smp, struct lru64 *lru)
{
	struct chunk *cache_entry = lru->data;
	smp->data.u.str.str = cache_entry->str;
	smp->data.u.str.len = cache_entry->len;
}
#endif

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
/* Sets the important HTTP headers ahead of the detection
 */
static void _51d_set_headers(struct sample *smp, fiftyoneDegreesWorkset *ws)
{
	struct hdr_idx *idx;
	struct hdr_ctx ctx;
	const struct http_msg *msg;
	int i;

	idx = &smp->strm->txn->hdr_idx;
	msg = &smp->strm->txn->req;

	ws->importantHeadersCount = 0;

	for (i = 0; i < global_51degrees.header_count; i++) {
		ctx.idx = 0;
		if (http_find_full_header2(
			(global_51degrees.header_names + i)->str,
			(global_51degrees.header_names + i)->len,
			msg->chn->buf->p, idx, &ctx) == 1) {
			ws->importantHeaders[ws->importantHeadersCount].header = ws->dataSet->httpHeaders + i;
			ws->importantHeaders[ws->importantHeadersCount].headerValue = ctx.line + ctx.val;
			ws->importantHeaders[ws->importantHeadersCount].headerValueLength = ctx.vlen;
			ws->importantHeadersCount++;
		}
	}
}
#endif

#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
static void _51d_set_device_offsets(struct sample *smp)
{
	struct hdr_idx *idx;
	struct hdr_ctx ctx;
	const struct http_msg *msg;
	int index;
	fiftyoneDegreesDeviceOffsets *offsets = &global_51degrees.device_offsets;

	idx = &smp->strm->txn->hdr_idx;
	msg = &smp->strm->txn->req;
	offsets->size = 0;

	for (index = 0; index < global_51degrees.header_count; index++) {
		ctx.idx = 0;
		if (http_find_full_header2(
			(global_51degrees.header_names + index)->str,
			(global_51degrees.header_names + index)->len,
			msg->chn->buf->p, idx, &ctx) == 1) {
			(offsets->firstOffset + offsets->size)->httpHeaderOffset = *(global_51degrees.header_offsets + index);
			(offsets->firstOffset + offsets->size)->deviceOffset = fiftyoneDegreesGetDeviceOffset(&global_51degrees.data_set, ctx.line + ctx.val);
			offsets->size++;
		}
	}
}
#endif

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
/* Provides a hash code for the important HTTP headers.
 */
unsigned long long _51d_req_hash(const struct arg *args, fiftyoneDegreesWorkset* ws)
{
	unsigned long long seed = _51d_lru_seed ^ (long)args;
	unsigned long long hash = 0;
	int i;
	for(i = 0; i < ws->importantHeadersCount; i++) {
		hash ^= ws->importantHeaders[i].header->headerNameOffset;
		hash ^= XXH64(ws->importantHeaders[i].headerValue,
		              ws->importantHeaders[i].headerValueLength,
		              seed);
	}
	return hash;
}
#endif

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
static void _51d_process_match(const struct arg *args, struct sample *smp, fiftyoneDegreesWorkset* ws)
{
	char *methodName;
#endif
#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
static void _51d_process_match(const struct arg *args, struct sample *smp)
{
	char valuesBuffer[1024];
	const char **requiredProperties = fiftyoneDegreesGetRequiredPropertiesNames(&global_51degrees.data_set);
	int requiredPropertiesCount = fiftyoneDegreesGetRequiredPropertiesCount(&global_51degrees.data_set);
	fiftyoneDegreesDeviceOffsets *deviceOffsets = &global_51degrees.device_offsets;

#endif

	char no_data[] = "NoData";  /* response when no data could be found */
	struct chunk *temp = get_trash_chunk();
	int j, i = 0, found;
	const char* property_name;

	/* Loop through property names passed to the filter and fetch them from the dataset. */
	while (args[i].data.str.str) {
		/* Try to find request property in dataset. */
		found = 0;
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
		if (strcmp("Method", args[i].data.str.str) == 0) {
			switch(ws->method) {
				case EXACT: methodName = "Exact"; break;
				case NUMERIC: methodName = "Numeric"; break;
				case NEAREST: methodName = "Nearest"; break;
				case CLOSEST: methodName = "Closest"; break;
				default:
				case NONE: methodName = "None"; break;
			}
			chunk_appendf(temp, "%s", methodName);
			found = 1;
		}
		else if (strcmp("Difference", args[i].data.str.str) == 0) {
			chunk_appendf(temp, "%d", ws->difference);
			found = 1;
		}
		else if (strcmp("Rank", args[i].data.str.str) == 0) {
			chunk_appendf(temp, "%d", fiftyoneDegreesGetSignatureRank(ws));
			found = 1;
		}
		else {
			for (j = 0; j < ws->dataSet->requiredPropertyCount; j++) {
				property_name = fiftyoneDegreesGetPropertyName(ws->dataSet, ws->dataSet->requiredProperties[j]);
				if (strcmp(property_name, args[i].data.str.str) == 0) {
					found = 1;
					fiftyoneDegreesSetValues(ws, j);
					chunk_appendf(temp, "%s", fiftyoneDegreesGetValueName(ws->dataSet, *ws->values));
					break;
				}
			}
		}
#endif
#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
		found = 0;
		for (j = 0; j < requiredPropertiesCount; j++) {
			property_name = requiredProperties[j];
			if (strcmp(property_name, args[i].data.str.str) == 0 &&
				fiftyoneDegreesGetValueFromOffsets(&global_51degrees.data_set, deviceOffsets, j, valuesBuffer, 1024) > 0) {
				found = 1;
				chunk_appendf(temp, "%s", valuesBuffer);
				break;
			}
		}
#endif
		if (!found)
			chunk_appendf(temp, "%s", no_data);

		/* Add separator. */
		chunk_appendf(temp, "%c", global_51degrees.property_separator);
		++i;
	}

	if (temp->len) {
		--temp->len;
		temp->str[temp->len] = '\0';
	}

	smp->data.u.str.str = temp->str;
	smp->data.u.str.len = temp->len;
}

static int _51d_fetch(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	fiftyoneDegreesWorkset* ws; /* workset for detection */
	struct lru64 *lru = NULL;
#endif

	/* Needed to ensure that the HTTP message has been fully recieved when
	 * used with TCP operation. Not required for HTTP operation.
	 * Data type has to be reset to ensure the string output is processed
	 * correctly.
	 */
	CHECK_HTTP_MESSAGE_FIRST();
	smp->data.type = SMP_T_STR;

	/* Flags the sample to show it uses constant memory*/
	smp->flags |= SMP_F_CONST;

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED

	/* Get only the headers needed for device detection so they can be used
	 * with the cache to return previous results. Pattern is slower than
	 * Trie so caching will help improve performance.
	 */

	/* Get a workset from the pool which will later contain detection results. */
	ws = fiftyoneDegreesWorksetPoolGet(global_51degrees.pool);
	if (!ws)
		return 0;

	/* Set the important HTTP headers for this request in the workset. */
	_51d_set_headers(smp, ws);

	/* Check the cache to see if there's results for these headers already. */
	if (_51d_lru_tree) {
		lru = lru64_get(_51d_req_hash(args, ws),
		                _51d_lru_tree, (void*)args, 0);
		if (lru && lru->domain) {
			fiftyoneDegreesWorksetPoolRelease(global_51degrees.pool, ws);
			_51d_retrieve_cache_entry(smp, lru);
			return 1;
		}
	}

	fiftyoneDegreesMatchForHttpHeaders(ws);

	_51d_process_match(args, smp, ws);

#endif

#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED

	/* Trie is very fast so all the headers can be passed in and the result
	 * returned faster than the hashing algorithm process.
	 */
	_51d_set_device_offsets(smp);
	_51d_process_match(args, smp);

#endif

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	fiftyoneDegreesWorksetPoolRelease(global_51degrees.pool, ws);
	if (lru)
		_51d_insert_cache_entry(smp, lru, (void*)args);
#endif

	return 1;
}

static int _51d_conv(const struct arg *args, struct sample *smp, void *private)
{
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	fiftyoneDegreesWorkset* ws; /* workset for detection */
	struct lru64 *lru = NULL;
#endif

	/* Flags the sample to show it uses constant memory*/
	smp->flags |= SMP_F_CONST;

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED

	/* Look in the list. */
	if (_51d_lru_tree) {
		unsigned long long seed = _51d_lru_seed ^ (long)args;

		lru = lru64_get(XXH64(smp->data.u.str.str, smp->data.u.str.len, seed),
		                _51d_lru_tree, (void*)args, 0);
		if (lru && lru->domain) {
			_51d_retrieve_cache_entry(smp, lru);
			return 1;
		}
	}

	/* Create workset. This will later contain detection results. */
	ws = fiftyoneDegreesWorksetPoolGet(global_51degrees.pool);
	if (!ws)
		return 0;
#endif

	/* Duplicate the data and remove the "const" flag before device detection. */
	if (!smp_dup(smp))
		return 0;

	smp->data.u.str.str[smp->data.u.str.len] = '\0';

	/* Perform detection. */
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	fiftyoneDegreesMatch(ws, smp->data.u.str.str);
	_51d_process_match(args, smp, ws);
#endif
#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
	global_51degrees.device_offsets.firstOffset->deviceOffset = fiftyoneDegreesGetDeviceOffset(&global_51degrees.data_set, smp->data.u.str.str);
	global_51degrees.device_offsets.size = 1;
	_51d_process_match(args, smp);
#endif

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	fiftyoneDegreesWorksetPoolRelease(global_51degrees.pool, ws);
	if (lru)
		_51d_insert_cache_entry(smp, lru, (void*)args);
#endif

	return 1;
}

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
void _51d_init_http_headers()
{
	int index = 0;
	const fiftyoneDegreesAsciiString *headerName;
	fiftyoneDegreesDataSet *ds = &global_51degrees.data_set;
	global_51degrees.header_count = ds->httpHeadersCount;
	global_51degrees.header_names = malloc(global_51degrees.header_count * sizeof(struct chunk));
	for (index = 0; index < global_51degrees.header_count; index++) {
		headerName = fiftyoneDegreesGetString(ds, ds->httpHeaders[index].headerNameOffset);
		(global_51degrees.header_names + index)->str = (char*)&headerName->firstByte;
		(global_51degrees.header_names + index)->len = headerName->length - 1;
		(global_51degrees.header_names + index)->size = (global_51degrees.header_names + index)->len;
	}
}
#endif

#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
void _51d_init_http_headers()
{
	int index = 0;
	fiftyoneDegreesDataSet *ds = &global_51degrees.data_set;
	global_51degrees.header_count = fiftyoneDegreesGetHttpHeaderCount(ds);
	global_51degrees.device_offsets.firstOffset = malloc(
		global_51degrees.header_count * sizeof(fiftyoneDegreesDeviceOffset));
	global_51degrees.header_names = malloc(global_51degrees.header_count * sizeof(struct chunk));
	global_51degrees.header_offsets = malloc(global_51degrees.header_count * sizeof(int32_t));
	for (index = 0; index < global_51degrees.header_count; index++) {
		global_51degrees.header_offsets[index] = fiftyoneDegreesGetHttpHeaderNameOffset(ds, index);
		global_51degrees.header_names[index].str = (char*)fiftyoneDegreesGetHttpHeaderNamePointer(ds, index);
		global_51degrees.header_names[index].len = strlen(global_51degrees.header_names[index].str);
		global_51degrees.header_names[index].size = global_51degrees.header_names[index].len;
	}
}
#endif

/*
 * module init / deinit functions. Returns 0 if OK, or a combination of ERR_*.
 */
static int init_51degrees(void)
{
	int i = 0;
	struct chunk *temp;
	struct _51d_property_names *name;
	char **_51d_property_list = NULL;
	fiftyoneDegreesDataSetInitStatus _51d_dataset_status = DATA_SET_INIT_STATUS_NOT_SET;

	if (!global_51degrees.data_file_path)
		return 0;

	if (global.nbthread > 1) {
		ha_alert("51Degrees: multithreading is not supported for now.\n");
		return (ERR_FATAL | ERR_ALERT);
	}

	if (!LIST_ISEMPTY(&global_51degrees.property_names)) {
		i = 0;
		list_for_each_entry(name, &global_51degrees.property_names, list)
			++i;
		_51d_property_list = calloc(i, sizeof(char *));

		i = 0;
		list_for_each_entry(name, &global_51degrees.property_names, list)
			_51d_property_list[i++] = name->name;
	}

	_51d_dataset_status = fiftyoneDegreesInitWithPropertyArray(global_51degrees.data_file_path, &global_51degrees.data_set, (const char**)_51d_property_list, i);

	temp = get_trash_chunk();
	chunk_reset(temp);

	switch (_51d_dataset_status) {
		case DATA_SET_INIT_STATUS_SUCCESS:
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
			/* only 1 workset in the pool because HAProxy is currently single threaded
			 * this value should be set to the number of threads in future versions.
			 */
			global_51degrees.pool = fiftyoneDegreesWorksetPoolCreate(&global_51degrees.data_set, NULL, 1);
#endif
			_51d_init_http_headers();
			break;
		case DATA_SET_INIT_STATUS_INSUFFICIENT_MEMORY:
			chunk_printf(temp, "Insufficient memory.");
			break;
		case DATA_SET_INIT_STATUS_CORRUPT_DATA:
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
			chunk_printf(temp, "Corrupt data file. Check that the data file provided is uncompressed and Pattern data format.");
#endif
#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
			chunk_printf(temp, "Corrupt data file. Check that the data file provided is uncompressed and Trie data format.");
#endif
			break;
		case DATA_SET_INIT_STATUS_INCORRECT_VERSION:
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
			chunk_printf(temp, "Incorrect version. Check that the data file provided is uncompressed and Pattern data format.");
#endif
#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
			chunk_printf(temp, "Incorrect version. Check that the data file provided is uncompressed and Trie data format.");
#endif
			break;
		case DATA_SET_INIT_STATUS_FILE_NOT_FOUND:
			chunk_printf(temp, "File not found.");
			break;
		case DATA_SET_INIT_STATUS_NULL_POINTER:
			chunk_printf(temp, "Null pointer to the existing dataset or memory location.");
			break;
		case DATA_SET_INIT_STATUS_POINTER_OUT_OF_BOUNDS:
			chunk_printf(temp, "Allocated continuous memory containing 51Degrees data file appears to be smaller than expected. Most likely"
			                   " because the data file was not fully loaded into the allocated memory.");
			break;
		case DATA_SET_INIT_STATUS_NOT_SET:
			chunk_printf(temp, "Data set not initialised.");
			break;
		default:
			chunk_printf(temp, "Other error.");
			break;
	}
	if (_51d_dataset_status != DATA_SET_INIT_STATUS_SUCCESS) {
		if (temp->len)
			ha_alert("51Degrees Setup - Error reading 51Degrees data file. %s\n", temp->str);
		else
			ha_alert("51Degrees Setup - Error reading 51Degrees data file.\n");
		return ERR_ALERT | ERR_FATAL;
	}
	free(_51d_property_list);

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	_51d_lru_seed = random();
	if (global_51degrees.cache_size) {
		_51d_lru_tree = lru64_new(global_51degrees.cache_size);
	}
#endif

	return 0;
}

static void deinit_51degrees(void)
{
	struct _51d_property_names *_51d_prop_name, *_51d_prop_nameb;

	free(global_51degrees.header_names);
#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	fiftyoneDegreesWorksetPoolFree(global_51degrees.pool);
#endif
#ifdef FIFTYONEDEGREES_H_TRIE_INCLUDED
	free(global_51degrees.device_offsets.firstOffset);
	free(global_51degrees.header_offsets);
#endif
	fiftyoneDegreesDataSetFree(&global_51degrees.data_set);

	free(global_51degrees.data_file_path); global_51degrees.data_file_path = NULL;
	list_for_each_entry_safe(_51d_prop_name, _51d_prop_nameb, &global_51degrees.property_names, list) {
		LIST_DEL(&_51d_prop_name->list);
		free(_51d_prop_name);
	}

#ifdef FIFTYONEDEGREES_H_PATTERN_INCLUDED
	while (lru64_destroy(_51d_lru_tree));
#endif
}

static struct cfg_kw_list _51dcfg_kws = {{ }, {
	{ CFG_GLOBAL, "51degrees-data-file", _51d_data_file },
	{ CFG_GLOBAL, "51degrees-property-name-list", _51d_property_name_list },
	{ CFG_GLOBAL, "51degrees-property-separator", _51d_property_separator },
	{ CFG_GLOBAL, "51degrees-cache-size", _51d_cache_size },
	{ 0, NULL, NULL },
}};

/* Note: must not be declared <const> as its list will be overwritten */
static struct sample_fetch_kw_list sample_fetch_keywords = {ILH, {
	{ "51d.all", _51d_fetch, ARG5(1,STR,STR,STR,STR,STR), _51d_fetch_check, SMP_T_STR, SMP_USE_HRQHV },
	{ NULL, NULL, 0, 0, 0 },
}};

/* Note: must not be declared <const> as its list will be overwritten */
static struct sample_conv_kw_list conv_kws = {ILH, {
	{ "51d.single", _51d_conv, ARG5(1,STR,STR,STR,STR,STR), _51d_conv_check, SMP_T_STR, SMP_T_STR },
	{ NULL, NULL, 0, 0, 0 },
}};

__attribute__((constructor))
static void __51d_init(void)
{
	/* register sample fetch and conversion keywords */
	sample_register_fetches(&sample_fetch_keywords);
	sample_register_convs(&conv_kws);
	cfg_register_keywords(&_51dcfg_kws);
	hap_register_build_opts("Built with 51Degrees support.", 0);
	hap_register_post_check(init_51degrees);
	hap_register_post_deinit(deinit_51degrees);
}
