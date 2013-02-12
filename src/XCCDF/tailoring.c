/*
 * Copyright 2009 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *      Martin Preisler <mpreisle@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "item.h"
#include "helpers.h"
#include "xccdf_impl.h"
#include "common/alloc.h"
#include "common/_error.h"
#include "common/debug_priv.h"

struct xccdf_tailoring *xccdf_tailoring_new(void)
{
	struct xccdf_tailoring *tailoring = oscap_calloc(1, sizeof(struct xccdf_tailoring));

	tailoring->benchmark_ref = NULL;

	tailoring->statuses = oscap_list_new();
	tailoring->dc_statuses = oscap_list_new();

	tailoring->version = NULL;
	tailoring->version_update = NULL;
	tailoring->version_time = NULL;

	tailoring->metadata = oscap_list_new();
	tailoring->profiles = oscap_list_new();

	return tailoring;
}

void xccdf_tailoring_free(struct xccdf_tailoring *tailoring)
{
	if (tailoring) {
		oscap_free(tailoring->benchmark_ref);

		oscap_list_free(tailoring->statuses, (oscap_destruct_func) xccdf_status_free);
		oscap_list_free(tailoring->dc_statuses, (oscap_destruct_func) oscap_reference_free);

		oscap_free(tailoring->version);
		oscap_free(tailoring->version_update);
		oscap_free(tailoring->version_time);

		oscap_list_free(tailoring->metadata, (oscap_destruct_func) oscap_free);
		oscap_list_free(tailoring->profiles, (oscap_destruct_func) xccdf_profile_free);

		oscap_free(tailoring);
	}
}

struct xccdf_tailoring *xccdf_tailoring_parse(xmlTextReaderPtr reader, struct xccdf_item *benchmark)
{
	XCCDF_ASSERT_ELEMENT(reader, XCCDFE_TAILORING);

	struct xccdf_tailoring *tailoring = xccdf_tailoring_new();

	int depth = oscap_element_depth(reader) + 1;

	// Read to the inside of Tailoring.
	xmlTextReaderRead(reader);

	while (oscap_to_start_element(reader, depth)) {
		switch (xccdf_element_get(reader)) {
		case XCCDFE_STATUS: {
			const char *date = xccdf_attribute_get(reader, XCCDFA_DATE);
			char *str = oscap_element_string_copy(reader);
			struct xccdf_status *status = xccdf_status_new_fill(str, date);
			oscap_free(str);
			oscap_list_add(tailoring->statuses, status);
			break;
		}
		case XCCDFE_DC_STATUS:
			oscap_list_add(tailoring->dc_statuses, oscap_reference_new_parse(reader));
			break;
		case XCCDFE_VERSION: {
			xmlNode *ver = xmlTextReaderExpand(reader);
			/* optional attributes */
			tailoring->version_time = (char*) xmlGetProp(ver, BAD_CAST "time");
			tailoring->version_update = (char*) xmlGetProp(ver, BAD_CAST "update");
			/* content */
			tailoring->version = (char *) xmlNodeGetContent(ver);
			if (oscap_streq(tailoring->version, "")) {
				oscap_free(tailoring->version);
				tailoring->version = NULL;
			}
			break;
		}
		case XCCDFE_METADATA: {
			char* xml = oscap_get_xml(reader);
			oscap_list_add(tailoring->metadata, oscap_strdup(xml));
			oscap_free(xml);
			break;
		}
		case XCCDFE_PROFILE: {
			struct xccdf_item *item = xccdf_profile_parse(reader, benchmark);
			xccdf_profile_set_tailoring(XPROFILE(item), true);
			oscap_list_add(tailoring->profiles, item);
			break;
		}
		default:
			dW("Encountered an unknown element '%s' while parsing XCCDF Tailoring element.",
				xmlTextReaderConstLocalName(reader));
		}
		xmlTextReaderRead(reader);
	}

	return tailoring;
}

struct xccdf_tailoring *xccdf_tailoring_import(const char *file, struct xccdf_benchmark *benchmark)
{
	xmlTextReaderPtr reader = xmlReaderForFile(file, NULL, 0);
	if (!reader) {
		oscap_seterr(OSCAP_EFAMILY_GLIBC, "Unable to open file: '%s'", file);
		return NULL;
	}
	while (xmlTextReaderRead(reader) == 1 && xmlTextReaderNodeType(reader) != 1) ;
	struct xccdf_tailoring *tailoring = xccdf_tailoring_parse(reader, XITEM(benchmark));
	xmlFreeTextReader(reader);

	if (!tailoring) { // parsing fatal error
		oscap_seterr(OSCAP_EFAMILY_XML, "Failed to parse '%s'.", file);
		xccdf_tailoring_free(tailoring);
		return NULL;
	}

	return tailoring;
}

xmlNodePtr xccdf_tailoring_to_dom(struct xccdf_tailoring *tailoring, xmlDocPtr doc, xmlNodePtr parent, const struct xccdf_version_info *version_info)
{
	xmlNs *ns_xccdf = xmlSearchNsByHref(doc, parent,
				(const xmlChar*)xccdf_version_info_get_namespace_uri(version_info));

	xmlNode *tailoring_node = NULL;
	tailoring_node = xmlNewTextChild(parent, ns_xccdf, BAD_CAST "Tailoring", NULL);

	struct xccdf_status_iterator *statuses = xccdf_tailoring_get_statuses(tailoring);
	while (xccdf_status_iterator_has_more(statuses)) {
		struct xccdf_status *status = xccdf_status_iterator_next(statuses);
		xccdf_status_to_dom(status, doc, tailoring_node, version_info);
	}
	xccdf_status_iterator_free(statuses);

	struct oscap_reference_iterator *dc_statuses = xccdf_tailoring_get_dc_statuses(tailoring);
	while (oscap_reference_iterator_has_more(dc_statuses)) {
		struct oscap_reference *ref = oscap_reference_iterator_next(dc_statuses);
		oscap_reference_to_dom(ref, tailoring_node, doc, "dc-status");
	}
	oscap_reference_iterator_free(dc_statuses);

	/* version and attributes */
	const char *version = xccdf_tailoring_get_version(tailoring);
	if (version) {
		xmlNode* version_node = xmlNewTextChild(tailoring_node, ns_xccdf, BAD_CAST "version", BAD_CAST version);

		const char *version_update = xccdf_tailoring_get_version_update(tailoring);
		if (version_update)
			xmlNewProp(version_node, BAD_CAST "update", BAD_CAST version_update);

		const char *version_time = xccdf_tailoring_get_version_time(tailoring);
		if (version_time)
			xmlNewProp(version_node, BAD_CAST "time", BAD_CAST version_time);
	}

	struct oscap_string_iterator* metadata = xccdf_tailoring_get_metadata(tailoring);
	while (oscap_string_iterator_has_more(metadata))
	{
		const char* meta = oscap_string_iterator_next(metadata);
		oscap_xmlstr_to_dom(tailoring_node, "metadata", meta);
	}
	oscap_string_iterator_free(metadata);

	struct xccdf_profile_iterator *profiles = xccdf_tailoring_get_profiles(tailoring);
	while (xccdf_profile_iterator_has_more(profiles)) {
		struct xccdf_profile *profile = xccdf_profile_iterator_next(profiles);
		xccdf_item_to_dom(XITEM(profile), doc, tailoring_node);
	}
	xccdf_profile_iterator_free(profiles);

	return tailoring_node;
}

const char *xccdf_tailoring_get_version(const struct xccdf_tailoring *tailoring)
{
	return tailoring->version;
}

const char *xccdf_tailoring_get_version_update(const struct xccdf_tailoring *tailoring)
{
	return tailoring->version_update;
}

const char *xccdf_tailoring_get_version_time(const struct xccdf_tailoring *tailoring)
{
	return tailoring->version_time;
}

struct oscap_string_iterator *xccdf_tailoring_get_metadata(const struct xccdf_tailoring *tailoring)
{
	return (struct oscap_string_iterator*) oscap_iterator_new(tailoring->metadata);
}

struct xccdf_profile_iterator *xccdf_tailoring_get_profiles(const struct xccdf_tailoring *tailoring)
{
	return (struct xccdf_profile_iterator*) oscap_iterator_new(tailoring->profiles);
}

struct xccdf_status_iterator *xccdf_tailoring_get_statuses(const struct xccdf_tailoring *tailoring)
{
	return (struct xccdf_status_iterator*) oscap_iterator_new(tailoring->statuses);
}

struct oscap_reference_iterator *xccdf_tailoring_get_dc_statuses(const struct xccdf_tailoring *tailoring)
{
	return (struct oscap_reference_iterator*) oscap_iterator_new(tailoring->dc_statuses);
}

struct xccdf_profile *
xccdf_tailoring_get_profile_by_id(const struct xccdf_tailoring *tailoring, const char *profile_id)
{
	struct xccdf_profile_iterator *profit = xccdf_tailoring_get_profiles(tailoring);
	while (xccdf_profile_iterator_has_more(profit)) {
		struct xccdf_profile *profile = xccdf_profile_iterator_next(profit);
		if (profile == NULL) {
			assert(profile != NULL);
			continue;
		}
		if (oscap_streq(xccdf_profile_get_id(profile), profile_id)) {
			xccdf_profile_iterator_free(profit);
			return profile;
		}
	}
	xccdf_profile_iterator_free(profit);
	return NULL;
}