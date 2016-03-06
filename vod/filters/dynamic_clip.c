#include "dynamic_clip.h"
#include "concat_clip.h"
#include "../media_set_parser.h"
#include "../parse_utils.h"

#define MAX_DYNAMIC_CLIP_SOURCE_COUNT (128)

// constants
static json_object_value_def_t dynamic_clip_params[] = {
	{ vod_string("id"), VOD_JSON_STRING, offsetof(media_clip_dynamic_t, id), media_set_parse_null_term_string },
	{ vod_null_string, 0, 0, NULL }
};

// globals
static vod_str_t dynamic_clip_no_mapping = vod_string("none");
static vod_hash_t dynamic_clip_hash;

vod_status_t
dynamic_clip_parse(
	void* ctx,
	vod_json_value_t* element,
	void** result)
{
	media_filter_parse_context_t* context = ctx;
	media_clip_dynamic_t* filter;
	vod_status_t rc;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"dynamic_clip_parse: started");

	filter = vod_alloc(context->request_context->pool, sizeof(*filter));
	if (filter == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
			"dynamic_clip_parse: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	filter->base.type = MEDIA_CLIP_DYNAMIC;
	filter->base.audio_filter = NULL;
	filter->base.sources = NULL;
	filter->base.source_count = 0;

	filter->id.len = 0;
	rc = vod_json_parse_object_values(
		element,
		&dynamic_clip_hash,
		context,
		filter);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (filter->id.len == 0)
	{
		vod_log_error(VOD_LOG_ERR, context->request_context->log, 0,
			"dynamic_clip_parse: \"id\" is mandatory for dynamic filter");
		return VOD_BAD_MAPPING;
	}

	filter->next = context->dynamic_clips_head;
	context->dynamic_clips_head = filter;

	filter->sequence = context->sequence;
	filter->sequence_offset = context->sequence_offset;
	filter->duration = context->duration;
	filter->range = context->range;

	*result = &filter->base;

	vod_log_debug0(VOD_LOG_DEBUG_LEVEL, context->request_context->log, 0,
		"dynamic_clip_parse: done");

	return VOD_OK;
}

vod_status_t
dynamic_clip_parser_init(
	vod_pool_t* pool,
	vod_pool_t* temp_pool)
{
	vod_status_t rc;

	rc = vod_json_init_hash(
		pool,
		temp_pool,
		"dynamic_clip_hash",
		dynamic_clip_params,
		sizeof(dynamic_clip_params[0]),
		&dynamic_clip_hash);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

vod_status_t
dynamic_clip_apply_mapping_json(
	media_clip_dynamic_t* clip, 
	request_context_t* request_context,
	u_char* mapping, 
	media_set_t* media_set)
{
	media_filter_parse_context_t context;
	vod_json_value_t json;
	media_clip_t* concat_clip;
	vod_status_t rc;
	u_char error[128];

	rc = vod_json_parse(request_context->pool, mapping, &json, error, sizeof(error));
	if (rc != VOD_JSON_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dynamic_clip_apply_mapping_json: failed to parse json %i: %s", rc, error);
		return VOD_BAD_MAPPING;
	}

	context.request_context = request_context;
	context.sources_head = media_set->sources_head;
	context.mapped_sources_head = media_set->mapped_sources_head;
	context.sequence = clip->sequence;
	context.sequence_offset = clip->sequence_offset;
	context.duration = clip->duration;
	context.range = clip->range;

	rc = concat_clip_parse(&context, &json, (void**)&concat_clip);
	if (rc != VOD_OK)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dynamic_clip_apply_mapping_json: concat_clip_parse failed %i", rc);
		return rc;
	}

	media_set->sources_head = context.sources_head;
	media_set->mapped_sources_head = context.mapped_sources_head;

	clip->base.type = MEDIA_CLIP_CONCAT;
	if (concat_clip->type == MEDIA_CLIP_SOURCE)
	{
		clip->base.sources = vod_alloc(request_context->pool, sizeof(clip->base.sources[0]));
		if (clip->base.sources == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"dynamic_clip_apply_mapping_json: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}
		clip->base.sources[0] = concat_clip;
		clip->base.source_count = 1;
	}
	else
	{
		clip->base.sources = concat_clip->sources;
		clip->base.source_count = concat_clip->source_count;
	}

	return VOD_OK;
}

vod_status_t
dynamic_clip_get_mapping_string(
	request_context_t* request_context,
	media_clip_dynamic_t* dynamic_clips_head, 
	vod_str_t* result)
{
	media_clip_dynamic_t* cur_clip;
	media_clip_source_t* cur_source;
	size_t alloc_size;
	uint32_t i;
	u_char* p;

	// calculate the alloc size
	alloc_size = 0;
	for (cur_clip = dynamic_clips_head; cur_clip != NULL; cur_clip = cur_clip->next)
	{
		if (cur_clip->base.source_count == 0)
		{
			continue;
		}

		alloc_size += sizeof("--") - 1 + VOD_INT32_LEN + cur_clip->id.len;

		for (i = 0; i < cur_clip->base.source_count; i++)
		{
			cur_source = (media_clip_source_t*)cur_clip->base.sources[i];
			alloc_size += sizeof("--") - 1 + VOD_INT64_LEN + cur_source->mapped_uri.len;
		}
	}

	if (alloc_size == 0)
	{
		*result = dynamic_clip_no_mapping;
		return VOD_OK;
	}

	// allocate the buffer
	p = vod_alloc(request_context->pool, alloc_size);
	if (p == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dynamic_clip_get_mapping_string: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	result->data = p;

	// build the value
	for (cur_clip = dynamic_clips_head; cur_clip != NULL; cur_clip = cur_clip->next)
	{
		if (cur_clip->base.source_count == 0)
		{
			continue;
		}

		if (p > result->data)
		{
			*p++ = '-';
		}

		p = vod_sprintf(p, "%V-%uD", &cur_clip->id, cur_clip->base.source_count);

		for (i = 0; i < cur_clip->base.source_count; i++)
		{
			cur_source = (media_clip_source_t*)cur_clip->base.sources[i];
			p = vod_sprintf(p, "-%V-%uL", 
				&cur_source->mapped_uri, 
				cur_source->sequence_offset - cur_clip->sequence_offset);
		}
	}

	*p = '\0';

	result->len = p - result->data;

	return VOD_OK;
}

static vod_status_t
dynamic_clip_extract_token(
	request_context_t* request_context, 
	u_char** cur, 
	u_char* end, 
	vod_str_t* result)
{
	u_char* p = *cur;

	result->data = p;
	p = memchr(p, '-', end - p);
	if (p == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dynamic_clip_extract_token: delimiter (-) not found");
		return VOD_BAD_REQUEST;
	}
	result->len = p - result->data;
	if (result->len <= 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dynamic_clip_extract_token: zero length token");
		return VOD_BAD_REQUEST;
	}

	*cur = p + 1;		// skip the -

	return VOD_OK;
}

static vod_status_t
dynamic_clip_apply_mapping_string_clip(
	request_context_t* request_context,
	media_set_t* media_set,
	media_clip_dynamic_t* clip, 
	uint32_t source_count,
	u_char** cur, 
	u_char* end)
{
	media_clip_source_t* sources_list_head;
	media_clip_source_t* cur_source;
	media_range_t* range;
	media_clip_t** cur_source_ptr;
	vod_str_t clip_id;
	uint32_t last_offset = 0;
	uint32_t offset;
	u_char* p = *cur;
	uint64_t range_start;
	uint64_t range_end;
	uint32_t i;
	vod_status_t rc;

	if (clip->range == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dynamic_clip_apply_mapping_string_clip: manifest request issued on a location with vod_apply_dynamic_mapping set");
		return VOD_BAD_REQUEST;
	}

	range_start = clip->range->start;
	range_end = clip->range->end;

	range = vod_alloc(request_context->pool, (sizeof(range[0]) + sizeof(cur_source[0]) + sizeof(cur_source_ptr[0])) * source_count);
	if (range == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dynamic_clip_apply_mapping_string_clip: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	cur_source = (void*)(range + source_count);
	vod_memzero(cur_source, sizeof(cur_source[0]) * source_count);

	cur_source_ptr = (void*)(cur_source + source_count);
	clip->base.sources = cur_source_ptr;

	sources_list_head = media_set->mapped_sources_head;

	for (i = 0; i < source_count; i++)
	{
		// clip id
		rc = dynamic_clip_extract_token(request_context, &p, end, &clip_id);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// offset
		p = parse_utils_extract_uint32_token(p, end, &offset);
		if (p < end)
		{
			if (*p != '-')
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"dynamic_clip_apply_mapping_string_clip: expected a delimiter (-) following the clip offset");
				return VOD_BAD_REQUEST;
			}

			p++;			// skip the -
		}

		// validate the ofset
		if (i > 0)
		{
			if (offset <= last_offset)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"dynamic_clip_apply_mapping_string_clip: current offset %uD is smaller than previous offset %uD", 
					offset, last_offset);
				return VOD_BAD_REQUEST;
			}

			if (offset <= range_start)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"dynamic_clip_apply_mapping_string_clip: current offset %uD is smaller than range start %uL",
					offset, range_start);
				return VOD_BAD_REQUEST;
			}
		}

		if (range_end <= offset)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"dynamic_clip_apply_mapping_string_clip: current offset %uD greater than range end %uL",
				offset, range_end);
			return VOD_BAD_REQUEST;
		}

		// calculate the range
		range->start = 0;
		range->timescale = 1000;

		if (i == 0)
		{
			if (range_start > offset)
			{
				range->start = range_start - offset;
			}
		}
		else
		{
			range[-1].end = offset - last_offset;
		}

		if (i + 1 == source_count)
		{
			range->end = range_end - offset;
		}

		// initialize the source
		cur_source->next = sources_list_head;
		sources_list_head = cur_source;

		cur_source->base.type = MEDIA_CLIP_SOURCE;

		cur_source->tracks_mask[MEDIA_TYPE_AUDIO] = 0xffffffff;
		cur_source->tracks_mask[MEDIA_TYPE_VIDEO] = 0xffffffff;
		cur_source->sequence = clip->sequence;
		cur_source->range = range;
		cur_source->sequence_offset = clip->sequence_offset + offset;
		cur_source->stripped_uri = cur_source->mapped_uri = clip_id;
		cur_source->clip_to = range->end;

		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dynamic_clip_apply_mapping_string_clip: parsed clip source - clipId=%V", &clip_id);

		*cur_source_ptr++ = &cur_source->base;
		cur_source++;
		range++;

		last_offset = offset;
	}

	media_set->mapped_sources_head = sources_list_head;

	// replace the dynamic clip with a concat filter
	clip->base.type = MEDIA_CLIP_CONCAT;
	clip->base.source_count = source_count;

	*cur = p;

	return VOD_OK;
}

vod_status_t
dynamic_clip_apply_mapping_string(
	request_context_t* request_context,
	media_set_t* media_set,
	vod_str_t* mapping)
{
	media_clip_dynamic_t** prev;
	media_clip_dynamic_t* cur;
	vod_str_t clip_id;
	vod_status_t rc;
	uint32_t source_count;
	u_char* end;
	u_char* p;

	// check for empty mapping
	if (mapping->len == dynamic_clip_no_mapping.len &&
		vod_memcmp(mapping->data, dynamic_clip_no_mapping.data, dynamic_clip_no_mapping.len) == 0)
	{
		return VOD_OK;
	}

	p = mapping->data;
	end = mapping->data + mapping->len;

	while (p < end)
	{
		// get the dynamic clip id
		rc = dynamic_clip_extract_token(request_context, &p, end, &clip_id);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// get the source count
		p = parse_utils_extract_uint32_token(p, end, &source_count);
		if (source_count <= 0 || source_count > MAX_DYNAMIC_CLIP_SOURCE_COUNT)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"dynamic_clip_apply_mapping_string: invalid dynamic clip source count %uD",
				source_count);
			return VOD_BAD_REQUEST;
		}

		if (p >= end || *p != '-')
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"dynamic_clip_apply_mapping_string: expected a delimiter (-) following the source count");
			return VOD_BAD_REQUEST;
		}
		p++;		// skip the -

		// look up the dynamic clip
		for (prev = &media_set->dynamic_clips_head; ; prev = &cur->next)
		{
			cur = *prev;
			if (cur == NULL)
			{
				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
					"dynamic_clip_apply_mapping_string: dynamic clip \"%V\" not found", &clip_id);
				return VOD_BAD_REQUEST;
			}

			if (cur->id.len == clip_id.len &&
				vod_memcmp(cur->id.data, clip_id.data, clip_id.len) == 0)
			{
				break;
			}
		}

		// apply the mapping
		rc = dynamic_clip_apply_mapping_string_clip(request_context, media_set, cur, source_count, &p, end);
		if (rc != VOD_OK)
		{
			return rc;
		}

		// remove the dynamic clip from the list
		*prev = cur->next;
	}

	return VOD_OK;
}
