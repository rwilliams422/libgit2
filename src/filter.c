/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "filter.h"

#include "common.h"
#include "futils.h"
#include "hash.h"
#include "repository.h"
#include "runtime.h"
#include "git2/sys/filter.h"
#include "git2/config.h"
#include "blob.h"
#include "attr_file.h"
#include "array.h"

struct git_filter_source {
	git_repository *repo;
	const char     *path;
	git_oid         oid;  /* zero if unknown (which is likely) */
	uint16_t        filemode; /* zero if unknown */
	git_filter_mode_t mode;
	uint32_t        flags;
};

typedef struct {
	const char *filter_name;
	git_filter *filter;
	void *payload;
} git_filter_entry;

struct git_filter_list {
	git_array_t(git_filter_entry) filters;
	git_filter_source source;
	git_buf *temp_buf;
	char path[GIT_FLEX_ARRAY];
};

typedef struct {
	char *filter_name;
	git_filter *filter;
	int priority;
	int initialized;
	size_t nattrs, nmatches;
	char *attrdata;
	const char *attrs[GIT_FLEX_ARRAY];
} git_filter_def;

static int filter_def_priority_cmp(const void *a, const void *b)
{
	int pa = ((const git_filter_def *)a)->priority;
	int pb = ((const git_filter_def *)b)->priority;
	return (pa < pb) ? -1 : (pa > pb) ? 1 : 0;
}

struct git_filter_registry {
	git_rwlock lock;
	git_vector filters;
};

static struct git_filter_registry filter_registry;

static void git_filter_global_shutdown(void);


static int filter_def_scan_attrs(
	git_buf *attrs, size_t *nattr, size_t *nmatch, const char *attr_str)
{
	const char *start, *scan = attr_str;
	int has_eq;

	*nattr = *nmatch = 0;

	if (!scan)
		return 0;

	while (*scan) {
		while (git__isspace(*scan)) scan++;

		for (start = scan, has_eq = 0; *scan && !git__isspace(*scan); ++scan) {
			if (*scan == '=')
				has_eq = 1;
		}

		if (scan > start) {
			(*nattr)++;
			if (has_eq || *start == '-' || *start == '+' || *start == '!')
				(*nmatch)++;

			if (has_eq)
				git_buf_putc(attrs, '=');
			git_buf_put(attrs, start, scan - start);
			git_buf_putc(attrs, '\0');
		}
	}

	return 0;
}

static void filter_def_set_attrs(git_filter_def *fdef)
{
	char *scan = fdef->attrdata;
	size_t i;

	for (i = 0; i < fdef->nattrs; ++i) {
		const char *name, *value;

		switch (*scan) {
		case '=':
			name = scan + 1;
			for (scan++; *scan != '='; scan++) /* find '=' */;
			*scan++ = '\0';
			value = scan;
			break;
		case '-':
			name = scan + 1; value = git_attr__false; break;
		case '+':
			name = scan + 1; value = git_attr__true;  break;
		case '!':
			name = scan + 1; value = git_attr__unset; break;
		default:
			name = scan;     value = NULL; break;
		}

		fdef->attrs[i] = name;
		fdef->attrs[i + fdef->nattrs] = value;

		scan += strlen(scan) + 1;
	}
}

static int filter_def_name_key_check(const void *key, const void *fdef)
{
	const char *name =
		fdef ? ((const git_filter_def *)fdef)->filter_name : NULL;
	return name ? git__strcmp(key, name) : -1;
}

static int filter_def_filter_key_check(const void *key, const void *fdef)
{
	const void *filter = fdef ? ((const git_filter_def *)fdef)->filter : NULL;
	return (key == filter) ? 0 : -1;
}

/* Note: callers must lock the registry before calling this function */
static int filter_registry_insert(
	const char *name, git_filter *filter, int priority)
{
	git_filter_def *fdef;
	size_t nattr = 0, nmatch = 0, alloc_len;
	git_buf attrs = GIT_BUF_INIT;

	if (filter_def_scan_attrs(&attrs, &nattr, &nmatch, filter->attributes) < 0)
		return -1;

	GIT_ERROR_CHECK_ALLOC_MULTIPLY(&alloc_len, nattr, 2);
	GIT_ERROR_CHECK_ALLOC_MULTIPLY(&alloc_len, alloc_len, sizeof(char *));
	GIT_ERROR_CHECK_ALLOC_ADD(&alloc_len, alloc_len, sizeof(git_filter_def));

	fdef = git__calloc(1, alloc_len);
	GIT_ERROR_CHECK_ALLOC(fdef);

	fdef->filter_name = git__strdup(name);
	GIT_ERROR_CHECK_ALLOC(fdef->filter_name);

	fdef->filter      = filter;
	fdef->priority    = priority;
	fdef->nattrs      = nattr;
	fdef->nmatches    = nmatch;
	fdef->attrdata    = git_buf_detach(&attrs);

	filter_def_set_attrs(fdef);

	if (git_vector_insert(&filter_registry.filters, fdef) < 0) {
		git__free(fdef->filter_name);
		git__free(fdef->attrdata);
		git__free(fdef);
		return -1;
	}

	git_vector_sort(&filter_registry.filters);
	return 0;
}

int git_filter_global_init(void)
{
	git_filter *crlf = NULL, *ident = NULL;
	int error = 0;

	if (git_rwlock_init(&filter_registry.lock) < 0)
		return -1;

	if ((error = git_vector_init(&filter_registry.filters, 2,
		filter_def_priority_cmp)) < 0)
		goto done;

	if ((crlf = git_crlf_filter_new()) == NULL ||
		filter_registry_insert(
			GIT_FILTER_CRLF, crlf, GIT_FILTER_CRLF_PRIORITY) < 0 ||
		(ident = git_ident_filter_new()) == NULL ||
		filter_registry_insert(
			GIT_FILTER_IDENT, ident, GIT_FILTER_IDENT_PRIORITY) < 0)
		error = -1;

	error = git_runtime_shutdown_register(git_filter_global_shutdown);

done:
	if (error) {
		git_filter_free(crlf);
		git_filter_free(ident);
	}

	return error;
}

static void git_filter_global_shutdown(void)
{
	size_t pos;
	git_filter_def *fdef;

	if (git_rwlock_wrlock(&filter_registry.lock) < 0)
		return;

	git_vector_foreach(&filter_registry.filters, pos, fdef) {
		if (fdef->filter && fdef->filter->shutdown) {
			fdef->filter->shutdown(fdef->filter);
			fdef->initialized = false;
		}

		git__free(fdef->filter_name);
		git__free(fdef->attrdata);
		git__free(fdef);
	}

	git_vector_free(&filter_registry.filters);

	git_rwlock_wrunlock(&filter_registry.lock);
	git_rwlock_free(&filter_registry.lock);
}

/* Note: callers must lock the registry before calling this function */
static int filter_registry_find(size_t *pos, const char *name)
{
	return git_vector_search2(
		pos, &filter_registry.filters, filter_def_name_key_check, name);
}

/* Note: callers must lock the registry before calling this function */
static git_filter_def *filter_registry_lookup(size_t *pos, const char *name)
{
	git_filter_def *fdef = NULL;

	if (!filter_registry_find(pos, name))
		fdef = git_vector_get(&filter_registry.filters, *pos);

	return fdef;
}


int git_filter_register(
	const char *name, git_filter *filter, int priority)
{
	int error;

	GIT_ASSERT_ARG(name);
	GIT_ASSERT_ARG(filter);

	if (git_rwlock_wrlock(&filter_registry.lock) < 0) {
		git_error_set(GIT_ERROR_OS, "failed to lock filter registry");
		return -1;
	}

	if (!filter_registry_find(NULL, name)) {
		git_error_set(
			GIT_ERROR_FILTER, "attempt to reregister existing filter '%s'", name);
		error = GIT_EEXISTS;
		goto done;
	}

	error = filter_registry_insert(name, filter, priority);

done:
	git_rwlock_wrunlock(&filter_registry.lock);
	return error;
}

int git_filter_unregister(const char *name)
{
	size_t pos;
	git_filter_def *fdef;
	int error = 0;

	GIT_ASSERT_ARG(name);

	/* cannot unregister default filters */
	if (!strcmp(GIT_FILTER_CRLF, name) || !strcmp(GIT_FILTER_IDENT, name)) {
		git_error_set(GIT_ERROR_FILTER, "cannot unregister filter '%s'", name);
		return -1;
	}

	if (git_rwlock_wrlock(&filter_registry.lock) < 0) {
		git_error_set(GIT_ERROR_OS, "failed to lock filter registry");
		return -1;
	}

	if ((fdef = filter_registry_lookup(&pos, name)) == NULL) {
		git_error_set(GIT_ERROR_FILTER, "cannot find filter '%s' to unregister", name);
		error = GIT_ENOTFOUND;
		goto done;
	}

	git_vector_remove(&filter_registry.filters, pos);

	if (fdef->initialized && fdef->filter && fdef->filter->shutdown) {
		fdef->filter->shutdown(fdef->filter);
		fdef->initialized = false;
	}

	git__free(fdef->filter_name);
	git__free(fdef->attrdata);
	git__free(fdef);

done:
	git_rwlock_wrunlock(&filter_registry.lock);
	return error;
}

static int filter_initialize(git_filter_def *fdef)
{
	int error = 0;

	if (!fdef->initialized && fdef->filter && fdef->filter->initialize) {
		if ((error = fdef->filter->initialize(fdef->filter)) < 0)
			return error;
	}

	fdef->initialized = true;
	return 0;
}

git_filter *git_filter_lookup(const char *name)
{
	size_t pos;
	git_filter_def *fdef;
	git_filter *filter = NULL;

	if (git_rwlock_rdlock(&filter_registry.lock) < 0) {
		git_error_set(GIT_ERROR_OS, "failed to lock filter registry");
		return NULL;
	}

	if ((fdef = filter_registry_lookup(&pos, name)) == NULL ||
		(!fdef->initialized && filter_initialize(fdef) < 0))
		goto done;

	filter = fdef->filter;

done:
	git_rwlock_rdunlock(&filter_registry.lock);
	return filter;
}

void git_filter_free(git_filter *filter)
{
	git__free(filter);
}

git_repository *git_filter_source_repo(const git_filter_source *src)
{
	return src->repo;
}

const char *git_filter_source_path(const git_filter_source *src)
{
	return src->path;
}

uint16_t git_filter_source_filemode(const git_filter_source *src)
{
	return src->filemode;
}

const git_oid *git_filter_source_id(const git_filter_source *src)
{
	return git_oid_is_zero(&src->oid) ? NULL : &src->oid;
}

git_filter_mode_t git_filter_source_mode(const git_filter_source *src)
{
	return src->mode;
}

uint32_t git_filter_source_flags(const git_filter_source *src)
{
	return src->flags;
}

static int filter_list_new(
	git_filter_list **out, const git_filter_source *src)
{
	git_filter_list *fl = NULL;
	size_t pathlen = src->path ? strlen(src->path) : 0, alloclen;

	GIT_ERROR_CHECK_ALLOC_ADD(&alloclen, sizeof(git_filter_list), pathlen);
	GIT_ERROR_CHECK_ALLOC_ADD(&alloclen, alloclen, 1);

	fl = git__calloc(1, alloclen);
	GIT_ERROR_CHECK_ALLOC(fl);

	if (src->path)
		memcpy(fl->path, src->path, pathlen);
	fl->source.repo = src->repo;
	fl->source.path = fl->path;
	fl->source.mode = src->mode;
	fl->source.flags = src->flags;

	*out = fl;
	return 0;
}

static int filter_list_check_attributes(
	const char ***out,
	git_repository *repo,
	git_attr_session *attr_session,
	git_filter_def *fdef,
	const git_filter_source *src)
{
	const char **strs = git__calloc(fdef->nattrs, sizeof(const char *));
	uint32_t flags = 0;
	size_t i;
	int error;

	GIT_ERROR_CHECK_ALLOC(strs);

	if ((src->flags & GIT_FILTER_NO_SYSTEM_ATTRIBUTES) != 0)
		flags |= GIT_ATTR_CHECK_NO_SYSTEM;

	if ((src->flags & GIT_FILTER_ATTRIBUTES_FROM_HEAD) != 0)
		flags |= GIT_ATTR_CHECK_INCLUDE_HEAD;

	error = git_attr_get_many_with_session(
		strs, repo, attr_session, flags, src->path, fdef->nattrs, fdef->attrs);

	/* if no values were found but no matches are needed, it's okay! */
	if (error == GIT_ENOTFOUND && !fdef->nmatches) {
		git_error_clear();
		git__free((void *)strs);
		return 0;
	}

	for (i = 0; !error && i < fdef->nattrs; ++i) {
		const char *want = fdef->attrs[fdef->nattrs + i];
		git_attr_value_t want_type, found_type;

		if (!want)
			continue;

		want_type  = git_attr_value(want);
		found_type = git_attr_value(strs[i]);

		if (want_type != found_type)
			error = GIT_ENOTFOUND;
		else if (want_type == GIT_ATTR_VALUE_STRING &&
				strcmp(want, strs[i]) &&
				strcmp(want, "*"))
			error = GIT_ENOTFOUND;
	}

	if (error)
		git__free((void *)strs);
	else
		*out = strs;

	return error;
}

int git_filter_list_new(
	git_filter_list **out,
	git_repository *repo,
	git_filter_mode_t mode,
	uint32_t flags)
{
	git_filter_source src = { 0 };
	src.repo = repo;
	src.path = NULL;
	src.mode = mode;
	src.flags = flags;
	return filter_list_new(out, &src);
}

int git_filter_list__load_ext(
	git_filter_list **filters,
	git_repository *repo,
	git_blob *blob, /* can be NULL */
	const char *path,
	git_filter_mode_t mode,
	git_filter_options *filter_opts)
{
	int error = 0;
	git_filter_list *fl = NULL;
	git_filter_source src = { 0 };
	git_filter_entry *fe;
	size_t idx;
	git_filter_def *fdef;

	if (git_rwlock_rdlock(&filter_registry.lock) < 0) {
		git_error_set(GIT_ERROR_OS, "failed to lock filter registry");
		return -1;
	}

	src.repo = repo;
	src.path = path;
	src.mode = mode;
	src.flags = filter_opts->flags;

	if (blob)
		git_oid_cpy(&src.oid, git_blob_id(blob));

	git_vector_foreach(&filter_registry.filters, idx, fdef) {
		const char **values = NULL;
		void *payload = NULL;

		if (!fdef || !fdef->filter)
			continue;

		if (fdef->nattrs > 0) {
			error = filter_list_check_attributes(
				&values, repo, filter_opts->attr_session, fdef, &src);

			if (error == GIT_ENOTFOUND) {
				error = 0;
				continue;
			} else if (error < 0)
				break;
		}

		if (!fdef->initialized && (error = filter_initialize(fdef)) < 0)
			break;

		if (fdef->filter->check)
			error = fdef->filter->check(
				fdef->filter, &payload, &src, values);

		git__free((void *)values);

		if (error == GIT_PASSTHROUGH)
			error = 0;
		else if (error < 0)
			break;
		else {
			if (!fl) {
				if ((error = filter_list_new(&fl, &src)) < 0)
					break;

				fl->temp_buf = filter_opts->temp_buf;
			}

			fe = git_array_alloc(fl->filters);
			GIT_ERROR_CHECK_ALLOC(fe);

			fe->filter = fdef->filter;
			fe->filter_name = fdef->filter_name;
			fe->payload = payload;
		}
	}

	git_rwlock_rdunlock(&filter_registry.lock);

	if (error && fl != NULL) {
		git_array_clear(fl->filters);
		git__free(fl);
		fl = NULL;
	}

	*filters = fl;
	return error;
}

int git_filter_list_load(
	git_filter_list **filters,
	git_repository *repo,
	git_blob *blob, /* can be NULL */
	const char *path,
	git_filter_mode_t mode,
	uint32_t flags)
{
	git_filter_options filter_opts = GIT_FILTER_OPTIONS_INIT;

	filter_opts.flags = flags;

	return git_filter_list__load_ext(
		filters, repo, blob, path, mode, &filter_opts);
}

void git_filter_list_free(git_filter_list *fl)
{
	uint32_t i;

	if (!fl)
		return;

	for (i = 0; i < git_array_size(fl->filters); ++i) {
		git_filter_entry *fe = git_array_get(fl->filters, i);
		if (fe->filter->cleanup)
			fe->filter->cleanup(fe->filter, fe->payload);
	}

	git_array_clear(fl->filters);
	git__free(fl);
}

int git_filter_list_contains(
	git_filter_list *fl,
	const char *name)
{
	size_t i;

	GIT_ASSERT_ARG(name);

	if (!fl)
		return 0;

	for (i = 0; i < fl->filters.size; i++) {
		if (strcmp(fl->filters.ptr[i].filter_name, name) == 0)
			return 1;
	}

	return 0;
}

int git_filter_list_push(
	git_filter_list *fl, git_filter *filter, void *payload)
{
	int error = 0;
	size_t pos;
	git_filter_def *fdef = NULL;
	git_filter_entry *fe;

	GIT_ASSERT_ARG(fl);
	GIT_ASSERT_ARG(filter);

	if (git_rwlock_rdlock(&filter_registry.lock) < 0) {
		git_error_set(GIT_ERROR_OS, "failed to lock filter registry");
		return -1;
	}

	if (git_vector_search2(
			&pos, &filter_registry.filters,
			filter_def_filter_key_check, filter) == 0)
		fdef = git_vector_get(&filter_registry.filters, pos);

	git_rwlock_rdunlock(&filter_registry.lock);

	if (fdef == NULL) {
		git_error_set(GIT_ERROR_FILTER, "cannot use an unregistered filter");
		return -1;
	}

	if (!fdef->initialized && (error = filter_initialize(fdef)) < 0)
		return error;

	fe = git_array_alloc(fl->filters);
	GIT_ERROR_CHECK_ALLOC(fe);
	fe->filter  = filter;
	fe->payload = payload;

	return 0;
}

size_t git_filter_list_length(const git_filter_list *fl)
{
	return fl ? git_array_size(fl->filters) : 0;
}

struct buf_stream {
	git_writestream parent;
	git_buf *target;
	bool complete;
};

static int buf_stream_write(
	git_writestream *s, const char *buffer, size_t len)
{
	struct buf_stream *buf_stream = (struct buf_stream *)s;
	GIT_ASSERT_ARG(buf_stream);
	GIT_ASSERT(buf_stream->complete == 0);

	return git_buf_put(buf_stream->target, buffer, len);
}

static int buf_stream_close(git_writestream *s)
{
	struct buf_stream *buf_stream = (struct buf_stream *)s;
	GIT_ASSERT_ARG(buf_stream);

	GIT_ASSERT(buf_stream->complete == 0);
	buf_stream->complete = 1;

	return 0;
}

static void buf_stream_free(git_writestream *s)
{
	GIT_UNUSED(s);
}

static void buf_stream_init(struct buf_stream *writer, git_buf *target)
{
	memset(writer, 0, sizeof(struct buf_stream));

	writer->parent.write = buf_stream_write;
	writer->parent.close = buf_stream_close;
	writer->parent.free = buf_stream_free;
	writer->target = target;

	git_buf_clear(target);
}

int git_filter_list_apply_to_data(
	git_buf *tgt, git_filter_list *filters, git_buf *src)
{
	struct buf_stream writer;
	int error;

	if ((error = git_buf_sanitize(tgt)) < 0 ||
	    (error = git_buf_sanitize(src)) < 0)
	    return error;

	if (!filters) {
		git_buf_attach_notowned(tgt, src->ptr, src->size);
		return 0;
	}

	buf_stream_init(&writer, tgt);

	if ((error = git_filter_list_stream_data(filters, src,
		&writer.parent)) < 0)
			return error;

	GIT_ASSERT(writer.complete);
	return error;
}

int git_filter_list_apply_to_file(
	git_buf *out,
	git_filter_list *filters,
	git_repository *repo,
	const char *path)
{
	struct buf_stream writer;
	int error;

	buf_stream_init(&writer, out);

	if ((error = git_filter_list_stream_file(
		filters, repo, path, &writer.parent)) < 0)
			return error;

	GIT_ASSERT(writer.complete);
	return error;
}

static int buf_from_blob(git_buf *out, git_blob *blob)
{
	git_object_size_t rawsize = git_blob_rawsize(blob);

	if (!git__is_sizet(rawsize)) {
		git_error_set(GIT_ERROR_OS, "blob is too large to filter");
		return -1;
	}

	git_buf_attach_notowned(out, git_blob_rawcontent(blob), (size_t)rawsize);
	return 0;
}

int git_filter_list_apply_to_blob(
	git_buf *out,
	git_filter_list *filters,
	git_blob *blob)
{
	struct buf_stream writer;
	int error;

	buf_stream_init(&writer, out);

	if ((error = git_filter_list_stream_blob(
		filters, blob, &writer.parent)) < 0)
			return error;

	GIT_ASSERT(writer.complete);
	return error;
}

struct proxy_stream {
	git_writestream parent;
	git_filter *filter;
	const git_filter_source *source;
	void **payload;
	git_buf input;
	git_buf temp_buf;
	git_buf *output;
	git_writestream *target;
};

static int proxy_stream_write(
	git_writestream *s, const char *buffer, size_t len)
{
	struct proxy_stream *proxy_stream = (struct proxy_stream *)s;
	GIT_ASSERT_ARG(proxy_stream);

	return git_buf_put(&proxy_stream->input, buffer, len);
}

static int proxy_stream_close(git_writestream *s)
{
	struct proxy_stream *proxy_stream = (struct proxy_stream *)s;
	git_buf *writebuf;
	git_error_state error_state = {0};
	int error;

	GIT_ASSERT_ARG(proxy_stream);

	error = proxy_stream->filter->apply(
		proxy_stream->filter,
		proxy_stream->payload,
		proxy_stream->output,
		&proxy_stream->input,
		proxy_stream->source);

	if (error == GIT_PASSTHROUGH) {
		writebuf = &proxy_stream->input;
	} else if (error == 0) {
		if ((error = git_buf_sanitize(proxy_stream->output)) < 0)
			return error;

		writebuf = proxy_stream->output;
	} else {
		/* close stream before erroring out taking care
		 * to preserve the original error */
		git_error_state_capture(&error_state, error);
		proxy_stream->target->close(proxy_stream->target);
		git_error_state_restore(&error_state);
		return error;
	}

	if ((error = proxy_stream->target->write(
			proxy_stream->target, writebuf->ptr, writebuf->size)) == 0)
		error = proxy_stream->target->close(proxy_stream->target);

	return error;
}

static void proxy_stream_free(git_writestream *s)
{
	struct proxy_stream *proxy_stream = (struct proxy_stream *)s;

	if (proxy_stream) {
		git_buf_dispose(&proxy_stream->input);
		git_buf_dispose(&proxy_stream->temp_buf);
		git__free(proxy_stream);
	}
}

static int proxy_stream_init(
	git_writestream **out,
	git_filter *filter,
	git_buf *temp_buf,
	void **payload,
	const git_filter_source *source,
	git_writestream *target)
{
	struct proxy_stream *proxy_stream = git__calloc(1, sizeof(struct proxy_stream));
	GIT_ERROR_CHECK_ALLOC(proxy_stream);

	proxy_stream->parent.write = proxy_stream_write;
	proxy_stream->parent.close = proxy_stream_close;
	proxy_stream->parent.free = proxy_stream_free;
	proxy_stream->filter = filter;
	proxy_stream->payload = payload;
	proxy_stream->source = source;
	proxy_stream->target = target;
	proxy_stream->output = temp_buf ? temp_buf : &proxy_stream->temp_buf;

	if (temp_buf)
		git_buf_clear(temp_buf);

	*out = (git_writestream *)proxy_stream;
	return 0;
}

static int stream_list_init(
	git_writestream **out,
	git_vector *streams,
	git_filter_list *filters,
	git_writestream *target)
{
	git_writestream *last_stream = target;
	size_t i;
	int error = 0;

	*out = NULL;

	if (!filters) {
		*out = target;
		return 0;
	}

	/* Create filters last to first to get the chaining direction */
	for (i = 0; i < git_array_size(filters->filters); ++i) {
		size_t filter_idx = (filters->source.mode == GIT_FILTER_TO_WORKTREE) ?
			git_array_size(filters->filters) - 1 - i : i;
		git_filter_entry *fe = git_array_get(filters->filters, filter_idx);
		git_writestream *filter_stream;

		GIT_ASSERT(fe->filter->stream || fe->filter->apply);

		/* If necessary, create a stream that proxies the traditional
		 * application.
		 */
		if (fe->filter->stream)
			error = fe->filter->stream(&filter_stream, fe->filter,
				&fe->payload, &filters->source, last_stream);
		else
			/* Create a stream that proxies the one-shot apply */
			error = proxy_stream_init(&filter_stream, fe->filter,
				filters->temp_buf, &fe->payload, &filters->source,
				last_stream);

		if (error < 0)
			goto out;

		git_vector_insert(streams, filter_stream);
		last_stream = filter_stream;
	}

out:
	if (error)
		last_stream->close(last_stream);
	else
		*out = last_stream;

	return error;
}

static void filter_streams_free(git_vector *streams)
{
	git_writestream *stream;
	size_t i;

	git_vector_foreach(streams, i, stream)
		stream->free(stream);
	git_vector_free(streams);
}

int git_filter_list_stream_file(
	git_filter_list *filters,
	git_repository *repo,
	const char *path,
	git_writestream *target)
{
	char buf[FILTERIO_BUFSIZE];
	git_buf abspath = GIT_BUF_INIT;
	const char *base = repo ? git_repository_workdir(repo) : NULL;
	git_vector filter_streams = GIT_VECTOR_INIT;
	git_writestream *stream_start;
	ssize_t readlen;
	int fd = -1, error, initialized = 0;

	if ((error = stream_list_init(
			&stream_start, &filter_streams, filters, target)) < 0 ||
	    (error = git_path_join_unrooted(&abspath, path, base, NULL)) < 0 ||
	    (error = git_path_validate_workdir_buf(repo, &abspath)) < 0)
		goto done;

	initialized = 1;

	if ((fd = git_futils_open_ro(abspath.ptr)) < 0) {
		error = fd;
		goto done;
	}

	while ((readlen = p_read(fd, buf, sizeof(buf))) > 0) {
		if ((error = stream_start->write(stream_start, buf, readlen)) < 0)
			goto done;
	}

	if (readlen < 0)
		error = -1;

done:
	if (initialized)
		error |= stream_start->close(stream_start);

	if (fd >= 0)
		p_close(fd);
	filter_streams_free(&filter_streams);
	git_buf_dispose(&abspath);
	return error;
}

int git_filter_list_stream_data(
	git_filter_list *filters,
	git_buf *data,
	git_writestream *target)
{
	git_vector filter_streams = GIT_VECTOR_INIT;
	git_writestream *stream_start;
	int error, initialized = 0;

	if ((error = git_buf_sanitize(data)) < 0)
		return error;

	if ((error = stream_list_init(&stream_start, &filter_streams, filters, target)) < 0)
		goto out;
	initialized = 1;

	if ((error = stream_start->write(
			stream_start, data->ptr, data->size)) < 0)
		goto out;

out:
	if (initialized)
		error |= stream_start->close(stream_start);

	filter_streams_free(&filter_streams);
	return error;
}

int git_filter_list_stream_blob(
	git_filter_list *filters,
	git_blob *blob,
	git_writestream *target)
{
	git_buf in = GIT_BUF_INIT;

	if (buf_from_blob(&in, blob) < 0)
		return -1;

	if (filters)
		git_oid_cpy(&filters->source.oid, git_blob_id(blob));

	return git_filter_list_stream_data(filters, &in, target);
}

int git_filter_init(git_filter *filter, unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(filter, version, git_filter, GIT_FILTER_INIT);
	return 0;
}
