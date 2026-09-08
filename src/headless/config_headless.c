/*
  RPCEmu - An Acorn system emulator

  Headless configuration loading.

  Reads the same rpc.cfg the Qt frontend uses, with the same keys, defaults
  and fallbacks — QSettings' IniFormat is a plain INI file, and unprefixed
  keys live in its [General] section.

  config_save() deliberately does nothing by default. The headless build is
  a tool driven by an agent; rewriting the machine's configuration as a side
  effect of exiting would make runs non-repeatable and could disturb the
  interactive build's setup. Pass --save-config to opt in.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpcemu.h"
#include "headless.h"

#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif

#define MAX_KEYS	64
#define MAX_KEY_LEN	64
#define MAX_VAL_LEN	512

typedef struct {
	char key[MAX_KEY_LEN];
	char val[MAX_VAL_LEN];
} CfgEntry;

static CfgEntry entries[MAX_KEYS];
static int entry_count;

int headless_save_config;	/**< Set by --save-config */

/**
 * Trim leading and trailing whitespace in place.
 */
static char *
trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}

	end = s + strlen(s);
	while (end > s) {
		const char c = end[-1];

		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
			break;
		}
		end--;
	}
	*end = '\0';

	return s;
}

/**
 * Read rpc.cfg into the entry table.
 *
 * Only the [General] section (and any keys before the first section header)
 * is collected, matching how QSettings stores unprefixed keys.
 */
static void
cfg_read(const char *filename)
{
	char line[1024];
	FILE *f;
	int in_general = 1;

	entry_count = 0;

	f = fopen(filename, "r");
	if (f == NULL) {
		rpclog("config_load: could not open '%s', using defaults\n", filename);
		return;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char *s = trim(line);
		char *eq;

		if (*s == '\0' || *s == '#' || *s == ';') {
			continue;
		}

		if (*s == '[') {
			in_general = (strncmp(s, "[General]", 9) == 0);
			continue;
		}

		if (!in_general) {
			continue;
		}

		eq = strchr(s, '=');
		if (eq == NULL) {
			continue;
		}
		*eq = '\0';

		if (entry_count < MAX_KEYS) {
			CfgEntry *e = &entries[entry_count++];

			snprintf(e->key, sizeof(e->key), "%s", trim(s));
			snprintf(e->val, sizeof(e->val), "%s", trim(eq + 1));

			rpclog("config_load: %s = \"%s\"\n", e->key, e->val);
		}
	}

	fclose(f);
}

static const char *
cfg_str(const char *key, const char *fallback)
{
	int i;

	for (i = 0; i < entry_count; i++) {
		if (strcasecmp(entries[i].key, key) == 0) {
			return entries[i].val;
		}
	}

	return fallback;
}

static int
cfg_int(const char *key, int fallback)
{
	const char *v = cfg_str(key, NULL);

	return (v != NULL && *v != '\0') ? atoi(v) : fallback;
}

/**
 * Duplicate a config string, or return NULL for an empty value.
 */
static char *
cfg_strdup(const char *key)
{
	const char *v = cfg_str(key, "");

	return (*v != '\0') ? strdup(v) : NULL;
}

void
config_load(Config *config)
{
	char filename[512];
	const char *p;
	Model model;
	int i;

	snprintf(filename, sizeof(filename), "%srpc.cfg", rpcemu_get_datadir());
	cfg_read(filename);

	/* Memory size: only the sizes the hardware supports, else 16MB */
	switch (cfg_int("mem_size", 16)) {
	case 4:   config->mem_size = 4;   break;
	case 8:   config->mem_size = 8;   break;
	case 32:  config->mem_size = 32;  break;
	case 64:  config->mem_size = 64;  break;
	case 128: config->mem_size = 128; break;
	case 256: config->mem_size = 256; break;
	default:  config->mem_size = 16;  break;
	}

	p = cfg_str("vram_size", "");
	config->vram_size = (strcmp(p, "0") == 0) ? 0 : 8;

	p = cfg_str("model", "");
	model = Model_RPCARM710;
	for (i = 0; i < Model_MAX; i++) {
		if (strcasecmp(p, models[i].name_config) == 0) {
			model = (Model) i;
			break;
		}
	}

	rpcemu_model_changed(model);

	/* A7000 and A7000+ have no VRAM */
	if (model == Model_A7000 || model == Model_A7000plus) {
		config->vram_size = 0;
	}

	/* Phoebe has a fixed configuration */
	if (model == Model_Phoebe) {
		config->mem_size = 256;
		config->vram_size = 4;
	}

	config->soundenabled = cfg_int("sound_enabled", 1);
	config->refresh      = cfg_int("refresh_rate", 60);
	config->cdromenabled = cfg_int("cdrom_enabled", 0);
	config->cdromtype    = cfg_int("cdrom_type", 0);

	p = cfg_str("cdrom_iso", "");
	if (snprintf(config->isoname, sizeof(config->isoname), "%s", p) >=
	    (int) sizeof(config->isoname))
	{
		rpclog("config_load: cdrom_iso path too long - ignored\n");
		config->isoname[0] = '\0';
	}

	config->mousehackon    = cfg_int("mouse_following", 1);
	config->mousetwobutton = cfg_int("mouse_twobutton", 0);

	p = cfg_str("network_type", "off");
	if (strcasecmp(p, "off") == 0) {
		config->network_type = NetworkType_Off;
	} else if (strcasecmp(p, "nat") == 0) {
		config->network_type = NetworkType_NAT;
	} else if (strcasecmp(p, "iptunnelling") == 0) {
		config->network_type = NetworkType_IPTunnelling;
	} else if (strcasecmp(p, "ethernetbridging") == 0) {
		config->network_type = NetworkType_EthernetBridging;
	} else {
		rpclog("Unknown network_type '%s', defaulting to off\n", p);
		config->network_type = NetworkType_Off;
	}

#ifndef RPCEMU_NETWORKING
	if (config->network_type != NetworkType_Off) {
		rpclog("config_load: networking not built in, forcing off\n");
		config->network_type = NetworkType_Off;
	}
#endif

	config->username   = cfg_strdup("username");
	config->ipaddress  = cfg_strdup("ipaddress");
	config->macaddress = cfg_strdup("macaddress");
	config->bridgename = cfg_strdup("bridgename");

	config->cpu_idle                = cfg_int("cpu_idle", 0);
	config->show_fullscreen_message = cfg_int("show_fullscreen_message", 1);
	config->network_capture         = cfg_strdup("network_capture");

	/* No NAT rules are loaded: the headless build does not do networking
	   yet, and an agent-controlled machine should not reach the outside
	   world by accident. */
	memset(port_forward_rules, 0, sizeof(port_forward_rules));
}

void
config_save(Config *config)
{
	NOT_USED(config);

	if (!headless_save_config) {
		rpclog("config_save: skipped (headless build does not rewrite rpc.cfg)\n");
		return;
	}

	rpclog("config_save: --save-config given, but saving is not implemented;"
	       " edit rpc.cfg directly\n");
}
