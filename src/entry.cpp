/*
 * App2Clap
 * Plug-in entry point
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025 James Teh
 * License: GNU General Public License version 2.0
 */

#include <string.h>

#include "clap/clap.h"

extern const clap_plugin_descriptor app2ClapDescriptor;
const clap_plugin* createApp2Clap(const clap_host* host);

extern const clap_plugin_descriptor clap2AppDescriptor;
const clap_plugin* createClap2App(const clap_host* host);

static const clap_plugin_factory factory = {
	.get_plugin_count = [] (const clap_plugin_factory* factory) -> uint32_t {
		return 2;
	},
	.get_plugin_descriptor = [] (const clap_plugin_factory* factory, uint32_t index) -> const clap_plugin_descriptor* {
		if (index == 0) {
			return &app2ClapDescriptor;
		}
		if (index == 1) {
			return &clap2AppDescriptor;
		}
		return nullptr;
	},
	.create_plugin = [] (const clap_plugin_factory* factory, const clap_host* host, const char *pluginID) -> const clap_plugin* {
		if (strcmp(pluginID, app2ClapDescriptor.id) == 0) {
			return createApp2Clap(host);
		}
		if (strcmp(pluginID, clap2AppDescriptor.id) == 0) {
			return createClap2App(host);
		}
		return nullptr;
	},
};

CLAP_EXPORT const clap_plugin_entry clap_entry = {
	.clap_version = CLAP_VERSION,
	.init = [] (const char *path) -> bool { return true; },
	.deinit = [] () {},
	.get_factory = [] (const char *factoryID) -> const void * {
		return strcmp(factoryID, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
	},
};
